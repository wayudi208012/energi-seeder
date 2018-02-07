#include <algorithm>

#include "db.h"
#include "netbase.h"
#include "protocol.h"
#include "serialize.h"
#include "uint256.h"

#define BITCOIN_SEED_NONCE  0x0539a019ca550825ULL

using namespace std;

class CNode {
  CAddress you;
  unsigned int nHeaderStart;
  unsigned int nMessageStart;
  vector<CAddress> *vAddr;
  int ban;
  int64 doneAfter;
  int nVersion;
  SOCKET sock;
  CDataStream vSend;
  CDataStream vRecv;
  string strSubVer;
  int nStartingHeight;

  int GetTimeout() {
      if (you.IsTor())
          return 120;
      else
          return 30;
  }

  void BeginMessage(const char *pszCommand) {
    if (nHeaderStart != -1) AbortMessage();
    nHeaderStart = vSend.size();
    vSend << CMessageHeader(pszCommand, 0);
    nMessageStart = vSend.size();
    #ifdef DEBUG_PRINT
    printf("%s: SEND %s\n", ToString(you).c_str(), pszCommand);
    #endif
  }

  void AbortMessage() {
    if (nHeaderStart == -1) return;
    vSend.resize(nHeaderStart);
    nHeaderStart = -1;
    nMessageStart = -1;
  }

  void EndMessage() {
    if (nHeaderStart == -1) return;
    unsigned int nSize = vSend.size() - nMessageStart;
    memcpy((char*)&vSend[nHeaderStart] + offsetof(CMessageHeader, nMessageSize), &nSize, sizeof(nSize));
    if (vSend.GetVersion() >= 209) {
      uint256 hash = Hash(vSend.begin() + nMessageStart, vSend.end());
      unsigned int nChecksum = 0;
      memcpy(&nChecksum, &hash, sizeof(nChecksum));
      assert(nMessageStart - nHeaderStart >= offsetof(CMessageHeader, nChecksum) + sizeof(nChecksum));
      memcpy((char*)&vSend[nHeaderStart] + offsetof(CMessageHeader, nChecksum), &nChecksum, sizeof(nChecksum));
    }
    nHeaderStart = -1;
    nMessageStart = -1;
  }

  void Send() {
    if (sock == INVALID_SOCKET) return;
    if (vSend.empty()) return;
    int nBytes = send(sock, &vSend[0], vSend.size(), 0);
    if (nBytes > 0) {
      vSend.erase(vSend.begin(), vSend.begin() + nBytes);
    } else {
      close(sock);
      sock = INVALID_SOCKET;
    }
  }

  void PushVersion() {
    int64 nTime = time(NULL);
    uint64 nLocalNonce = BITCOIN_SEED_NONCE;
    int64 nLocalServices = 0;
    CAddress me(CService("0.0.0.0"));
    BeginMessage("version");
    int nBestHeight = GetRequireHeight();
    string ver = "/energi-seeder:0.0.1/";
    vSend << PROTOCOL_VERSION << nLocalServices << nTime << you << me << nLocalNonce << ver << nBestHeight;
    EndMessage();
  }

  void GotVersion() {
    #ifdef DEBUG_PRINT
    printf("\n%s: version %i\n", ToString(you).c_str(), nVersion);
    #endif
    if (vAddr) {
      BeginMessage("getaddr");
      EndMessage();
      doneAfter = time(NULL) + GetTimeout();
    } else {
      doneAfter = time(NULL) + 1;
    }
  }

  bool ProcessMessage(string strCommand, CDataStream& vRecv) {
    #ifdef DEBUG_PRINT
    printf("%s: RECV %s\n", ToString(you).c_str(), strCommand.c_str());
    #endif
    if (strCommand == "version") {
      int64 nTime;
      CAddress addrMe;
      CAddress addrFrom;
      uint64 nNonce = 1;
      vRecv >> nVersion >> you.nServices >> nTime >> addrMe;
      if (nVersion == 10300) nVersion = 300;
      if (nVersion >= 106 && !vRecv.empty())
        vRecv >> addrFrom >> nNonce;
      if (nVersion >= 106 && !vRecv.empty())
        vRecv >> strSubVer;
      if (nVersion >= 209 && !vRecv.empty())
        vRecv >> nStartingHeight;

      if (nVersion >= 209) {
        BeginMessage("verack");
        EndMessage();
      }
      vSend.SetVersion(min(nVersion, PROTOCOL_VERSION));
      if (nVersion < 209) {
        this->vRecv.SetVersion(min(nVersion, PROTOCOL_VERSION));
        GotVersion();
      }
      return false;
    }

    if (strCommand == "verack") {
      this->vRecv.SetVersion(min(nVersion, PROTOCOL_VERSION));
      GotVersion();
      return false;
    }

    if (strCommand == "addr" && vAddr) {
      vector<CAddress> vAddrNew;
      vRecv >> vAddrNew;
      #ifdef DEBUG_PRINT
      printf("%s: got %i addresses\n", ToString(you).c_str(), (int)vAddrNew.size());
      #endif
      int64 now = time(NULL);
      vector<CAddress>::iterator it = vAddrNew.begin();
      if (vAddrNew.size() > 1) {
        if (doneAfter == 0 || doneAfter > now + 1) doneAfter = now + 1;
      }
      while (it != vAddrNew.end()) {
        CAddress &addr = *it;
        #ifdef DEBUG_PRINT
        printf("%s: got address %s size %d\n", ToString(you).c_str(), addr.ToString().c_str(), (int)(vAddr->size()));
        #endif
        it++;
        if (addr.nTime <= 100000000 || addr.nTime > now + 600)
          addr.nTime = now - 5 * 86400;
        if (addr.nTime > now - 604800)
          vAddr->push_back(addr);
        #ifdef DEBUG_PRINT
        printf("%s: added address %s (#%i)\n", ToString(you).c_str(), addr.ToString().c_str(), (int)(vAddr->size()));
        #endif
        if (vAddr->size() > 1000) {doneAfter = 1; return true; }
      }
      return false;
    }

    return false;
  }

  bool ProcessMessages() {
    if (vRecv.empty()) return false;
    do {
      CDataStream::iterator pstart = search(vRecv.begin(), vRecv.end(), BEGIN(pchMessageStart), END(pchMessageStart));
      int nHeaderSize = vRecv.GetSerializeSize(CMessageHeader());
      if (vRecv.end() - pstart < nHeaderSize) {
        if (vRecv.size() > nHeaderSize) {
          vRecv.erase(vRecv.begin(), vRecv.end() - nHeaderSize);
        }
        break;
      }
      vRecv.erase(vRecv.begin(), pstart);
      vector<char> vHeaderSave(vRecv.begin(), vRecv.begin() + nHeaderSize);
      CMessageHeader hdr;
      vRecv >> hdr;
      if (!hdr.IsValid()) {
        #ifdef DEBUG_PRINT
        printf("%s: BAD (invalid header)\n", ToString(you).c_str());
        #endif
        ban = 100000; return true;
      }
      string strCommand = hdr.GetCommand();
      unsigned int nMessageSize = hdr.nMessageSize;
      if (nMessageSize > MAX_SIZE) {
        #ifdef DEBUG_PRINT
        printf("%s: BAD (message too large)\n", ToString(you).c_str());
        #endif
        ban = 100000;
        return true;
      }
      if (nMessageSize > vRecv.size()) {
        vRecv.insert(vRecv.begin(), vHeaderSave.begin(), vHeaderSave.end());
        break;
      }
      if (vRecv.GetVersion() >= 209) {
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum) continue;
      }
      CDataStream vMsg(vRecv.begin(), vRecv.begin() + nMessageSize, vRecv.nType, vRecv.nVersion);
      vRecv.ignore(nMessageSize);
      if (ProcessMessage(strCommand, vMsg))
        return true;

      #ifdef DEBUG_PRINT
      printf("%s: done processing %s\n", ToString(you).c_str(), strCommand.c_str());
      #endif
    } while(1);
    return false;
  }

public:
  CNode(const CService& ip, vector<CAddress>* vAddrIn)
  : you(ip)
  , nHeaderStart(-1)
  , nMessageStart(-1)
  , vAddr(vAddrIn)
  , ban(0)
  , doneAfter(0)
  , nVersion(0)
  , sock(INVALID_SOCKET)
  , vSend()
  , vRecv()
  , strSubVer()
  , nStartingHeight(0)
  {
    vSend.SetType(SER_NETWORK);
    vSend.SetVersion(0);
    vRecv.SetType(SER_NETWORK);
    vRecv.SetVersion(0);
    if (time(NULL) > 1329696000) {
      vSend.SetVersion(209);
      vRecv.SetVersion(209);
    }
  }
  bool Run() {
    bool res = true;
    if (!ConnectSocket(you, sock)) return false;
    PushVersion();
    Send();
    int64 now;
    while (now = time(NULL), ban == 0 && (doneAfter == 0 || doneAfter > now) && sock != INVALID_SOCKET) {
      char pchBuf[0x10000];
      fd_set set;
      FD_ZERO(&set);
      FD_SET(sock,&set);
      struct timeval wa;
      if (doneAfter) {
        wa.tv_sec = doneAfter - now;
        wa.tv_usec = 0;
      } else {
        wa.tv_sec = GetTimeout();
        wa.tv_usec = 0;
      }
      int ret = select(sock+1, &set, NULL, &set, &wa);
      if (ret != 1) {
        if (!doneAfter) res = false;
        break;
      }
      int nBytes = recv(sock, pchBuf, sizeof(pchBuf), 0);
      int nPos = vRecv.size();
      if (nBytes > 0) {
        vRecv.resize(nPos + nBytes);
        memcpy(&vRecv[nPos], pchBuf, nBytes);
      } else if (nBytes == 0) {
        #ifdef DEBUG_PRINT
        printf("%s: BAD (connection closed prematurely)\n", ToString(you).c_str());
        #endif
        res = false;
        break;
      } else {
        #ifdef DEBUG_PRINT
        printf("%s: BAD (connection error)\n", ToString(you).c_str());
        #endif
        res = false;
        break;
      }
      ProcessMessages();
      Send();
    }
    if (sock == INVALID_SOCKET) res = false;
    close(sock);
    sock = INVALID_SOCKET;
    return (ban == 0) && res;
  }

  int GetBan() {
    return ban;
  }

  int GetClientVersion() {
    return nVersion;
  }

  std::string GetClientSubVersion() {
    return strSubVer;
  }

  int GetStartingHeight() {
    return nStartingHeight;
  }
};

bool TestNode(const CService &cip, int &ban, int &clientV, std::string &clientSV, int &blocks, vector<CAddress>* vAddr) {
  try {
    CNode node(cip, vAddr);
    bool ret = node.Run();
    if (!ret) {
      ban = node.GetBan();
      #ifdef DEBUG_PRINT
      printf("node.Run() failed\n");
      #endif
    } else {
      ban = 0;
    }
    clientV = node.GetClientVersion();
    clientSV = node.GetClientSubVersion();
    blocks = node.GetStartingHeight();
    #ifdef DEBUG_PRINT
    printf("TestNode() tested %s version %d subversion %s blockheight %d\n", cip.ToString().c_str(), clientV, clientSV.c_str(), blocks);
    printf("%s: %s!!!\n", cip.ToString().c_str(), ret ? "GOOD" : "BAD");
    #endif
    return ret;
  } catch(std::ios_base::failure& e) {
    ban = 0;
    return false;
  }
}

