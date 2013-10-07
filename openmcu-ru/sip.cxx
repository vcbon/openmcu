#include <ptlib.h>
#include "mcu.h"
#include <sys/types.h>
#ifndef _WIN32
#  include <sys/socket.h>
#endif

PString GetFromIp(const char *toAddr, const char *toPort)
{
    char buffer[16];
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock == -1) return NULL;

    struct addrinfo serv;
    struct addrinfo *res=NULL;
    memset((void *)&serv, 0, sizeof(serv));
    serv.ai_family = AF_INET;
    serv.ai_socktype=SOCK_DGRAM;
    int err=getaddrinfo(toAddr, toPort, &serv, &res);
    if (err != 0 || res == NULL) return NULL;

    err = connect(sock, res->ai_addr, res->ai_addrlen);
    if(err == -1) return NULL;

    sockaddr_in name;
    socklen_t namelen = sizeof(name);
    err = getsockname(sock, (sockaddr*) &name, &namelen);
    if(err == -1) return NULL;

#ifndef _WIN32
    inet_ntop(AF_INET, (const void *)&name.sin_addr, buffer, 16);
    close(sock);
    return (PString)buffer;
#else
    PString buffer0 = PIPSocket::Address(name.sin_addr);
    close(sock);
    return buffer0;
#endif
}

unsigned GetLocalDataPort(PString localIP, unsigned portBase, unsigned portMax)
{
  unsigned localDataPort = portBase;
  PQoS * dataQos = NULL;
  PQoS * ctrlQos = NULL;
  PUDPSocket * dataSocket = new PUDPSocket(dataQos);
  PUDPSocket * controlSocket = new PUDPSocket(ctrlQos);
  while(!dataSocket->Listen(localIP, 1, localDataPort) || !controlSocket->Listen(localIP, 1, localDataPort+1))
  {
    dataSocket->Close();
    controlSocket->Close();
    if ((localDataPort > portMax) || (localDataPort > 0xfffd))
      return 0;
    localDataPort += 2;
  }
  delete dataSocket;
  delete controlSocket;
  return localDataPort;
}

PString CreateSdpInvite()
{
 PString types, map, type, name, fmtp;
 PStringArray caps;
 PINDEX tsNum = 0; while(OpenMCU::Current().GetEndpoint().tsCaps[tsNum]!=NULL) { caps.AppendString(OpenMCU::Current().GetEndpoint().tsCaps[tsNum]); tsNum++; }
 PINDEX tvNum = 0; while(OpenMCU::Current().GetEndpoint().tvCaps[tvNum]!=NULL) { caps.AppendString(OpenMCU::Current().GetEndpoint().tvCaps[tvNum]); tvNum++; }
 tsNum--; tvNum--;

 PString sdp =
        "v=0\n"
        "o=USERNAME 1806 3221 IN IP4 LOCALIP\n"
        "s=Talk\n"
        "c=IN IP4 LOCALIP\n"
        "t=0 0\n";

 for (PINDEX i = 0; caps[i] != NULL; i++)
 {
   H323Capability *cap = H323Capability::Create(caps[i]);
   if(cap == NULL && caps[i].Find("{sw}") == P_MAX_INDEX)
     cap = H323Capability::Create(caps[i]+"{sw}");
   if(cap)
   {
     const OpalMediaFormat & mf = cap->GetMediaFormat();
     if(i <= tsNum) type = i+100; else type = i+100-tsNum-1;
     name = PString(cap->GetFormatName()).ToLower();
     if(name.Find("ulaw") != P_MAX_INDEX) name = "pcmu";
     if(name.Find("alaw") != P_MAX_INDEX) name = "pcma";
     name = name.Left(PString(cap->GetFormatName()).Find("-"))
            .Left(name.Find("_"))+"/"+PString(mf.GetTimeUnits()*1000);
     name.Replace(".","",TRUE,0);
     if(name.Find("h263p") != P_MAX_INDEX) name.Replace("h263p","h263-1998",TRUE,0);
     if(mf.GetOptionInteger("Encoder Channels") == 2 || mf.GetOptionInteger("Decoder Channels") == 2) goto end;

     PString fmtp = "";
     if(MCUConfig("CODEC_OPTIONS").HasKey(cap->GetFormatName()))
     {
       fmtp = MCUConfig("CODEC_OPTIONS").GetString(cap->GetFormatName());
     //} else {
     //  for (PINDEX j = 0; j < mf.GetOptionCount(); j++)
     //    if(mf.GetOption(j).GetFMTPName() != "" && mf.GetOption(j).GetFMTPDefault() != mf.GetOption(j).AsString())
     //      fmtp += mf.GetOption(j).GetFMTPName()+"="+mf.GetOption(j).AsString()+";";
     }
     fmtp += "\r\n";
     if(map.Find(name) != P_MAX_INDEX && map.Find(fmtp) != P_MAX_INDEX) goto end;
     if(map.Find(name) != P_MAX_INDEX && cap->GetMainType() != 0) goto end;

     types += type+" ";
     map += "a=rtpmap:"+type+" "+name+"\r\n";
     if(fmtp != "\r\n" && i <= tsNum) map += "a=fmtp:"+type+" "+fmtp;
   }

   end:
   if(i == tsNum)
     { sdp += "m=audio RTP_AUDIO_PORT RTP/AVP "+types+"\r\n"+map; map=""; types=""; }
 }
 sdp += "m=video RTP_VIDEO_PORT RTP/AVP "+types+"\r\n"+map;
 cout << sdp;
 return sdp;
}

void OpenMCUSipConnection::LeaveConference()
{
 PString *bye = new PString("BYE");
 cmdQueue.Push(bye); // Queue is not thread safe for multiple writers, so connection must be locked before call this
 LeaveConference(FALSE);
}

void OpenMCUSipConnection::LeaveConference(BOOL remove)
{
//  PWaitAndSignal m(connMutex);
 PTRACE(1, "MCUSIP\tLeaveConference " << remove);
 if(remove == FALSE) return;
 OpenMCUH323Connection::LeaveConference();
}

RTP_UDP *OpenMCUSipConnection::CreateRTPSession(int pt, SipCapability *sc)
{
  int id = (!sc->media)?RTP_Session::DefaultAudioSessionID:RTP_Session::DefaultVideoSessionID;
  RTP_UDP * session = (RTP_UDP *)(rtpSessions.UseSession(id));
  if(session == NULL)
  {
   session = new RTP_UDP(
#ifdef H323_RTP_AGGREGATE
                useRTPAggregation ? endpoint.GetRTPAggregator() : NULL,
#endif    
                id, remoteIsNAT);
   rtpSessions.AddSession(session);
   PIPSocket::Address lIP(localIP); 
   PIPSocket::Address rIP(remoteIP);
   unsigned portBase, portMax;
   if(audioRtpPort && videoRtpPort)
   {
     if(!sc->media)
     {
       portBase = audioRtpPort;
       portMax = audioRtpPort;
     } else {
       portBase = videoRtpPort;
       portMax = videoRtpPort;
     }
   } else {
     portBase = endpoint.GetRtpIpPortBase();
     portMax = endpoint.GetRtpIpPortMax();
   }
   session->Open(lIP,portBase,portMax,endpoint.GetRtpIpTypeofService(),*this,NULL,NULL);
   session->SetRemoteSocketInfo(rIP,sc->port,TRUE);
   sc->lport = session->GetLocalDataPort();
   sc->sdp = PString("m=") + ((!sc->media)?"audio ":"video ")
           + PString(sc->lport) + " RTP/AVP " + PString(pt) + "\r\n";
   if(sc->bandwidth) sc->sdp = sc->sdp + "b=AS:" + PString(sc->bandwidth) + "\r\n";
   if(sc->dir == 3) sc->sdp = sc->sdp + "a=sendrecv\r\n";
   else if(sc->dir == 1) sc->sdp = sc->sdp + "a=sendonly\r\n";
   else if(sc->dir == 2) sc->sdp = sc->sdp + "a=recvonly\r\n";
   else if(sc->dir == 0) sc->sdp = sc->sdp + "a=inactive\r\n";
   if(pt != 0 && pt != 8)
   {
    sc->sdp = sc->sdp + "a=rtpmap:" + PString(pt) + " " + sc->format + "/" + PString(sc->clock);
    if(sc->cnum) sc->sdp = sc->sdp + "/" + PString(sc->cnum);
    sc->sdp = sc->sdp + "\r\n";
    if(sc->cap)
    {
      PString fmtp = "";
      if(MCUConfig("CODEC_OPTIONS").HasKey(sc->h323))
        fmtp = MCUConfig("CODEC_OPTIONS").GetString(sc->h323);
      else // send incoming(from the client) ftmp
        fmtp = sc->parm;
      if(fmtp != "")
        sc->sdp = sc->sdp + "a=fmtp:" + PString(pt) + " " + fmtp + "\r\n";
    }
   }
   sdp_msg += sc->sdp;
  }
  return session;
}

int OpenMCUSipConnection::CreateAudioChannel(int pt, int dir)
{
 SipCapMapType::iterator cir = sipCaps.find(pt);
 PString h323Name = cir->second->h323;
 H323Capability * cap = cir->second->cap;
 if(cap!=NULL)
 {
  RTP_UDP *session = CreateRTPSession(pt, cir->second);
  H323_RTPChannel *channel = 
     new H323_RTPChannel(*this, *cap, (!dir)?H323Channel::IsReceiver:H323Channel::IsTransmitter, *session);
  if (pt >= RTP_DataFrame::DynamicBase && pt <= RTP_DataFrame::MaxPayloadType)  
   channel->SetDynamicRTPPayloadType(pt);
  if(!dir) cir->second->inpChan = channel; else cir->second->outChan = channel;
 }
 return 0;
}

int OpenMCUSipConnection::CreateVideoChannel(int pt, int dir)
{
 SipCapMapType::iterator cir = sipCaps.find(pt);
 PString h323Name = cir->second->h323;
 H323Capability * cap = cir->second->cap;
 if(cap!=NULL)
 {
  RTP_UDP *session = CreateRTPSession(pt, cir->second);
  H323_RTPChannel *channel = 
     new H323_RTPChannel(*this, *cap, (!dir)?H323Channel::IsReceiver:H323Channel::IsTransmitter, *session);
  if (pt >= RTP_DataFrame::DynamicBase && pt <= RTP_DataFrame::MaxPayloadType)  
   channel->SetDynamicRTPPayloadType(pt);
  if(!dir) cir->second->inpChan = channel; else cir->second->outChan = channel;
 }
 return 0;
}

void OpenMCUSipConnection::CreateLogicalChannels()
{
 if(scap >= 0) // audio capability is set
 {
  CreateAudioChannel(scap,0);
  CreateAudioChannel(scap,1);
 }
 if(vcap >= 0) // video capability is set
 {
  CreateVideoChannel(vcap,0);
  CreateVideoChannel(vcap,1);
 }
}

void OpenMCUSipConnection::StartChannel(int pt, int dir)
{
 if(pt<0) return;
 SipCapMapType::iterator cir = sipCaps.find(pt);
 if(dir == 0 && (cir->second->dir&2) && cir->second->inpChan && !cir->second->inpChan->IsRunning()) cir->second->inpChan->Start();
 if(dir == 1 && (cir->second->dir&1) && cir->second->outChan && !cir->second->outChan->IsRunning()) cir->second->outChan->Start();
}

void OpenMCUSipConnection::StartReceiveChannels()
{
 StartChannel(scap,0);
 StartChannel(vcap,0);
}

void OpenMCUSipConnection::StartTransmitChannels()
{
 StartChannel(scap,1);
 StartChannel(vcap,1);
}

void OpenMCUSipConnection::StopChannel(int pt, int dir)
{
 if(pt<0) return;
 SipCapMapType::iterator cir = sipCaps.find(pt);
 if(dir==0 && cir->second->inpChan) cir->second->inpChan->CleanUpOnTermination();
 if(dir==1 && cir->second->outChan) cir->second->outChan->CleanUpOnTermination();
}

void OpenMCUSipConnection::StopTransmitChannels()
{
 StopChannel(scap,1);
 StopChannel(vcap,1);
}

void OpenMCUSipConnection::StopReceiveChannels()
{
 StopChannel(scap,0);
 StopChannel(vcap,0);
}

void OpenMCUSipConnection::DeleteMediaChannels(int pt)
{
 if(pt<0) return;
 SipCapMapType::iterator cir = sipCaps.find(pt);
 if(cir->second->inpChan) { delete cir->second->inpChan; cir->second->inpChan = NULL; }
 if(cir->second->outChan) { delete cir->second->outChan; cir->second->outChan = NULL; }
}

void OpenMCUSipConnection::DeleteChannels()
{
 DeleteMediaChannels(scap);
 DeleteMediaChannels(vcap);
}

void OpenMCUSipConnection::CleanUpOnCallEnd()
{
  PTRACE(1, "MCUSIP\tCleanUpOnCallEnd");
  StopTransmitChannels();
  StopReceiveChannels();
  DeleteChannels();
  videoReceiveCodecName = videoTransmitCodecName = "none";
  videoReceiveCodec = NULL;
  videoTransmitCodec = NULL;
}

void SipCapability::Print()
{
 cout << "Payload: " << payload << " Media: " << media << " Direction: " << dir << " Port: " << port << "\r\n";
 cout << "Clock: " << clock << " Bandwidth: " << bandwidth << "\r\n";
 cout << "Format: " << format << "\r\n";
 cout << "Parameters: " << parm << "\r\n\r\n";
}


void OpenMCUSipConnection::FindCapability_H263(SipCapability &c,PStringArray &keys, const char * _H323Name, const char * _SIPName)
{
 PString H323Name(_H323Name);
 PString SIPName(_SIPName);
 for(int kn=0; kn<keys.GetSize(); kn++) 
 { 
  if(keys[kn].Find(SIPName + "=")==0)
   { 
    c.cap = H323Capability::Create(H323Name);
    if(c.cap == NULL) return;
    vcap = c.payload; c.h323 = H323Name; c.parm += keys[kn]; 
    OpalMediaFormat & wf = c.cap->GetWritableMediaFormat(); 
    int mpi = (keys[kn].Mid(SIPName.GetLength()+1)).AsInteger();
    cout << "mpi " << mpi << "\n";
    wf.SetOptionInteger(SIPName + " MPI",mpi);
    return; 
   } 
  }
}

void OpenMCUSipConnection::SelectCapability_H263(SipCapability &c,PStringArray &tvCaps)
{
 int f=0; // annex f
 PStringArray keys = c.parm.Tokenise(";");
 c.parm = "";
 for(int kn=0; kn<keys.GetSize(); kn++) 
  { if(keys[kn] == "F=1") { c.parm = "F=1;"; f=1; break; } }
 
 if(tvCaps.GetStringsIndex("H.263-16CIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263-16CIF{sw}","CIF16");
 if(tvCaps.GetStringsIndex("H.263-4CIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263-4CIF{sw}","CIF4");
 if(tvCaps.GetStringsIndex("H.263-CIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263-CIF{sw}","CIF");
 if(tvCaps.GetStringsIndex("H.263-QCIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263-QCIF{sw}","QCIF");
 if(tvCaps.GetStringsIndex("H.263-SQCIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263-SQCIF{sw}","SQCIF");

 if(c.cap)
 {
  OpalMediaFormat & wf = c.cap->GetWritableMediaFormat();
  wf.SetOptionBoolean("_advancedPrediction",f);
  if(c.bandwidth) wf.SetOptionInteger("Max Bit Rate",c.bandwidth*1000);
  else if(bandwidth) wf.SetOptionInteger("Max Bit Rate",bandwidth*1000);
 }
}


void OpenMCUSipConnection::SelectCapability_H263p(SipCapability &c,PStringArray &tvCaps)
{
 int f=0,d=0,e=0,g=0; // annexes
 PStringArray keys = c.parm.Tokenise(";");
 c.parm = "";
 for(int kn=0; kn<keys.GetSize(); kn++) 
 { 
  if(keys[kn] == "F=1") { c.parm += "F=1;"; f=1; } 
  else if(keys[kn] == "D=1") { c.parm += "D=1;"; d=1; } 
  else if(keys[kn] == "E=1") { c.parm += "E=1;"; e=1; } 
  else if(keys[kn] == "G=1") { c.parm += "G=1;"; g=1; } 
 }
 
 if(tvCaps.GetStringsIndex("H.263p-16CIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263p-16CIF{sw}","CIF16");
 if(tvCaps.GetStringsIndex("H.263p-4CIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263p-4CIF{sw}","CIF4");
 if(tvCaps.GetStringsIndex("H.263p-CIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263p-CIF{sw}","CIF");
 if(tvCaps.GetStringsIndex("H.263p-QCIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263p-QCIF{sw}","QCIF");
 if(tvCaps.GetStringsIndex("H.263p-SQCIF{sw}")!=P_MAX_INDEX && c.cap == NULL)
  FindCapability_H263(c,keys,"H.263p-SQCIF{sw}","SQCIF");

 if(c.cap)
 {
  OpalMediaFormat & wf = c.cap->GetWritableMediaFormat();
  wf.SetOptionBoolean("_advancedPrediction",f);
  wf.SetOptionBoolean("_unrestrictedVector",d);
  wf.SetOptionBoolean("_arithmeticCoding",e);
  wf.SetOptionBoolean("_pbFrames",g);
  if(c.bandwidth) wf.SetOptionInteger("Max Bit Rate",c.bandwidth*1000);
  else if(bandwidth) wf.SetOptionInteger("Max Bit Rate",bandwidth*1000);
 }
}

/*
packetization-mode=1;profile-level-id=42C01E 
*/

const struct h241_to_x264_level {
    int h241;
    int idc;
} h241_to_x264_levels[]=
{
    { 15, 9 },
    { 19,10 },
    { 22,11 },
    { 29,12 },
    { 36,13 },
    { 43,20 },
    { 50,21 },
    { 57,22 },
    { 64,30 },
    { 71,31 },
    { 78,32 },
    { 85,40 },
    { 92,41 },
    { 99,42 },
    { 106,50},
    { 113,51},
    { 0 }
};

void OpenMCUSipConnection::SelectCapability_H264(SipCapability &c,PStringArray &tvCaps)
{
 int profile = 0, level = 0;
 int max_mbps = 0, max_fs = 0, max_br = 0;
 PStringArray keys = c.parm.Tokenise(";");
 for(int kn = 0; kn < keys.GetSize(); kn++) 
 { 
  if(keys[kn].Find("profile-level-id=") == 0) 
  { 
   int p = (keys[kn].Tokenise("=")[1]).AsInteger(16);
   profile = (p>>16); level = (p&255);
  } 
  else if(keys[kn].Find("max-mbps=") == 0) 
  { 
   max_mbps = (keys[kn].Tokenise("=")[1]).AsInteger();
  } 
  else if(keys[kn].Find("max-fs=") == 0) 
  { 
   max_fs = (keys[kn].Tokenise("=")[1]).AsInteger();
  } 
  else if(keys[kn].Find("max-br=") == 0) 
  { 
   max_br = (keys[kn].Tokenise("=")[1]).AsInteger();
  } 
 }
 cout << "profile " << profile << " level " << level << "\n";
// if(profile == 0 || level == 0) return;
 if(level == 0)
 {
   PTRACE(1,"SIP_CONNECTION\tH.264 level will set to " << OpenMCU::Current().h264DefaultLevelForSip);
   level = OpenMCU::Current().h264DefaultLevelForSip;
 }
 int l = 0;
 while(h241_to_x264_levels[l].idc != 0)
 {
  if(level == h241_to_x264_levels[l].idc) { level = h241_to_x264_levels[l].h241; break; }
  l++;
 }
 profile = 64;

 cout << "profile " << profile << " level " << level << "\n";
 int cl = 0;
 for(int cn = 0; cn < tvCaps.GetSize(); cn++)
 {
  if(tvCaps[cn].Find("H.264")==0)
  {
   H323Capability *cap = H323Capability::Create(tvCaps[cn]);
   if(cap != NULL)
   {
    const OpalMediaFormat & mf = cap->GetMediaFormat(); 
    int flevel = mf.GetOptionInteger("Generic Parameter 42");
    cout << "flevel" << flevel << "\n";
    if(flevel > cl && flevel <= level) 
     { cl = flevel; if(c.cap) delete c.cap; c.cap = cap; c.h323 = tvCaps[cn]; }
    else { delete cap; }
    if(flevel == level) break; 
   }
  }
 }

 if(c.cap)
 {
  OpalMediaFormat & wf = c.cap->GetWritableMediaFormat();
  if(c.bandwidth) wf.SetOptionInteger("Max Bit Rate",c.bandwidth*1000);
  else if(bandwidth) wf.SetOptionInteger("Max Bit Rate",bandwidth*1000);
  wf.SetOptionInteger("Generic Parameter 42",level);
  vcap = c.payload;
  wf.SetOptionInteger("Generic Parameter 3",max_mbps);
  wf.SetOptionInteger("Generic Parameter 4",max_fs);
  wf.SetOptionInteger("Generic Parameter 6",max_br);
 }
}

void OpenMCUSipConnection::SelectCapability_VP8(SipCapability &c,PStringArray &tvCaps)
{
 int width = 0, height = 0;
 PStringArray keys = c.parm.Tokenise(";");
 for(int kn = 0; kn < keys.GetSize(); kn++)
 {
  if(keys[kn].Find("width=") == 0)
   width = (keys[kn].Tokenise("=")[1]).AsInteger();
  else if(keys[kn].Find("height=") == 0)
   height = (keys[kn].Tokenise("=")[1]).AsInteger();
 }

 PString H323Name;
 if (c.cap) c.cap=NULL;

 if (width && height)
 {
  for(int cn = 0; cn < tvCaps.GetSize(); cn++)
  {
   if(tvCaps[cn].Find("VP8")==0)
   {
    H323Name = tvCaps[cn];
    c.cap = H323Capability::Create(H323Name);
    if(c.cap)
    {
     const OpalMediaFormat & mf = c.cap->GetMediaFormat();
     if(width == mf.GetOptionInteger("Frame Width") && height == mf.GetOptionInteger("Frame Height"))
      break;
     else
      c.cap=NULL;
    }
   }
  }
  if(!c.cap && tvCaps.GetStringsIndex("VP8-CIF{sw}") != P_MAX_INDEX)
  {
   H323Name = "VP8-CIF{sw}";
   c.cap = H323Capability::Create(H323Name);
   if(c.cap)
   {
    OpalMediaFormat & wf = c.cap->GetWritableMediaFormat();
    wf.SetOptionInteger("Frame Width", width);
    wf.SetOptionInteger("Frame Height", height);
   }
  }
 }

 if(!c.cap && tvCaps.GetStringsIndex("VP8-CIF{sw}") != P_MAX_INDEX)
 {
  H323Name = "VP8-CIF{sw}";
  c.cap = H323Capability::Create(H323Name);
 }

 if(c.cap)
 {
  vcap = c.payload;
  c.h323 = H323Name;

  OpalMediaFormat & wf = c.cap->GetWritableMediaFormat();
  if(remoteApplication.ToLower().Find("linphone") == 0) wf.SetOptionEnum("Picture ID Size", 0);
  if(c.bandwidth) wf.SetOptionInteger("Max Bit Rate",c.bandwidth*1000);
  else if(bandwidth) wf.SetOptionInteger("Max Bit Rate",bandwidth*1000);
 }
}

void OpenMCUSipConnection::SelectCapability_SPEEX(SipCapability &c,PStringArray &tsCaps)
{
  int vbr = -1;
  int mode = -1;

  c.parm.Replace(" ","",TRUE,0);
  PStringArray keys = c.parm.Tokenise(";");
  for(int kn = 0; kn < keys.GetSize(); kn++)
  {
    if(keys[kn].Find("vbr=") == 0)
    {
      if(strcmp(keys[kn].Tokenise("=")[1], "on") == 0)
        vbr = 1;
      else if(strcmp(keys[kn].Tokenise("=")[1], "off") == 0)
        vbr = 0;
    }
    else if(keys[kn].Find("mode=") == 0)
      mode = (keys[kn].Tokenise("=")[1]).AsInteger();
  }

  PString H323Name;
  if(c.clock == 8000) H323Name = "Speex_8K{sw}";
  else if(c.clock == 16000) H323Name = "Speex_16K{sw}";
  else if(c.clock == 32000) H323Name = "Speex_32K{sw}";

  if(c.cap) c.cap = NULL;
  c.cap = H323Capability::Create(H323Name);
  if(c.cap)
  {
    scap = c.payload;
    c.h323 = H323Name;
    OpalMediaFormat & wf = c.cap->GetWritableMediaFormat();
    if (vbr > -1) wf.SetOptionEnum("vbr", vbr);
    if (mode > -1) wf.SetOptionInteger("mode", mode);
  }
}

void OpenMCUSipConnection::SelectCapability_OPUS(SipCapability &c,PStringArray &tsCaps)
{
  int cbr = -1;
  int maxaveragebitrate = -1;
  int useinbandfec = -1;
  int usedtx = -1;

  c.parm.Replace(" ","",TRUE,0);
  PStringArray keys = c.parm.Tokenise(";");
  for(int kn = 0; kn < keys.GetSize(); kn++)
  {
    if(keys[kn].Find("cbr=") == 0)
      cbr = (keys[kn].Tokenise("=")[1]).AsInteger();
    else if(keys[kn].Find("maxaveragebitrate=") == 0)
      maxaveragebitrate = (keys[kn].Tokenise("=")[1]).AsInteger();
    else if(keys[kn].Find("useinbandfec=") == 0)
      useinbandfec = (keys[kn].Tokenise("=")[1]).AsInteger();
    else if(keys[kn].Find("usedtx=") == 0)
      usedtx = (keys[kn].Tokenise("=")[1]).AsInteger();
  }

  PString H323Name;
  if(c.clock == 8000) H323Name = "OPUS_8K{sw}";
  else if(c.clock == 16000) H323Name = "OPUS_16K{sw}";
  else if(c.clock == 48000) H323Name = "OPUS_48K{sw}";

  if(c.cap) c.cap = NULL;
  c.cap = H323Capability::Create(H323Name);
  if(c.cap)
  {
   scap = c.payload;
   c.h323 = H323Name;
   OpalMediaFormat & wf = c.cap->GetWritableMediaFormat();
   if (cbr > -1) wf.SetOptionInteger("cbr", cbr);
   if (maxaveragebitrate > -1) wf.SetOptionInteger("maxaveragebitrate", maxaveragebitrate);
   if (useinbandfec > -1) wf.SetOptionInteger("useinbandfec", useinbandfec);
   if (usedtx > -1) wf.SetOptionInteger("usedtx", usedtx);
  }
}

int OpenMCUSipConnection::ProcessSDP(PStringArray &sdp_sa, PIntArray &par, SipCapMapType &caps, int reinvite)
{
 int par_len = 0, par_mbeg = 0;
 int port = -1, media = -1, def_dir = 3, dir = 3, bw = 0;
 for(int line=0; line<sdp_sa.GetSize(); line++)
 {
  char tag = sdp_sa[line][0];
  PStringArray words = sdp_sa[line].Tokenise(" ",FALSE);

  if(tag =='o')
  {
   if(reinvite)
   {
    if(words.GetSize() < 6) return 0; // wrong format
    if(words[1] != sess_id) return 0; // wrong sdp session id
    if(words[2].AsInteger() <= sess_ver.AsInteger()) return 0; // wrong sdp version
    // ok, this is actualy reinvite, lets handle it
    // we will not proccess the case of changing ip address
    sess_username = words[0];
    sess_id = words[1];
    sess_ver = words[2];
    continue;
   }
   if(words.GetSize() < 6) continue;
   sess_username = words[0];
   sess_id = words[1];
   sess_ver = words[2];
  }
  else if(tag =='c')
  {
   if(words.GetSize() != 3) return 0; // wrong format
   remoteIP = words[2];
  }
  else if(tag == 'm')
  {
   for(int cn=par_mbeg; cn<par.GetSize(); cn++)
   {
    SipCapMapType::iterator cir = caps.find(par[cn]);
    if(cir != caps.end()) { cir->second->dir = dir;  continue; } // payload found, here is nothing to do
    SipCapability *c = new SipCapability(par[cn],media,dir,port,bw);
    caps.insert(SipCapMapType::value_type(par[cn],c));
   }
   par_mbeg = par.GetSize();
   port = -1; media = -1; bw = 0; dir = def_dir; // reset media level values to default
   if(words.GetSize() < 4) continue; // empty fmt list
   if(words[2] != "RTP/AVP") continue; // non rtp media is not supported
   if(words[0].Find("audio")!=P_MAX_INDEX) media = 0;
   else if(words[0].Find("video")!=P_MAX_INDEX) media = 1;
   else media = 2;
   // double port from proxy server
   port = words[1].Left(5).AsInteger();
   //port = words[1].AsInteger();
   for(int wn = 3; wn < words.GetSize(); wn++) { par.SetAt(par_len,words[wn].AsInteger()); par_len++; }
  }
  else if(tag == 'a')
  {
   PString atag = words[0].Mid(2);
   if(atag == "recvonly")      { if(media < 0) def_dir = 1; dir = 1; }
   else if(atag == "sendonly") { if(media < 0) def_dir = 2; dir = 2; }
   else if(atag == "sendrecv") { if(media < 0) def_dir = 3; dir = 3; }
   else if(atag == "inactive") { if(media < 0) def_dir = 0; dir = 0; }
   else if(atag.Find("rtpmap")!=P_MAX_INDEX)
   {
    int payload;
    if(words.GetSize() < 2) continue; // invalid rtpmap string
    PStringArray tokens = atag.Tokenise(":");
    if(tokens.GetSize() < 2) continue; // invalid rtpmap string
    payload = tokens[1].AsInteger();
    tokens = words[1].Tokenise("/");
    if(tokens.GetSize() < 2) continue; // invalid rtpmap string
    SipCapability *c = new SipCapability(payload,media,dir,port,bw);
    c->format = tokens[0];
    c->clock = tokens[1].AsInteger();
    if(tokens.GetSize() == 3) c->cnum = tokens[2].AsInteger();
    caps.insert(SipCapMapType::value_type(payload,c));
   }
   else if(atag.Find("fmtp")!=P_MAX_INDEX)
   {
    int payload;
    if(words.GetSize() < 2) continue; // invalid fmtp string
    PStringArray tokens = atag.Tokenise(":");
    if(tokens.GetSize() < 2) continue; // invalid fmtp string
    payload = tokens[1].AsInteger();
    SipCapMapType::iterator cir = caps.find(payload);
    if(cir == caps.end()) continue; // payload is not exist
    cir->second->parm += words[1] + ";";
   }
  }
  else if(tag == 'b')
  {
   PStringArray tokens = words[0].Tokenise(":");
   if(tokens.GetSize() < 2) continue; // invalid bandwidth string
   if(tokens[0] == "b=AS") bw = tokens[1].AsInteger();
   if(tokens[0] == "b=TIAS") bw = tokens[1].AsInteger()/1000;
   if(media == -1) { bandwidth = bw; bw = 0; } // connection level value
  }
  cout << "line: " + sdp_sa[line] + "\r\n";
 } 

 for(int cn=par_mbeg; cn<par.GetSize(); cn++)
 {
  SipCapMapType::iterator cir = caps.find(par[cn]);
  if(cir != caps.end()) 
  {
   cir->second->dir = dir;
   cir->second->Print(); continue; 
  } // payload found
  SipCapability *c = new SipCapability(par[cn],media,dir,port,bw);
  caps.insert(SipCapMapType::value_type(par[cn],c));
  c->Print();
 }

 PStringArray tsCaps, tvCaps;
 int cn = 0; while(endpoint.tsCaps[cn]!=NULL) { tsCaps.AppendString(endpoint.tsCaps[cn]); cn++; }
 cn = 0; while(endpoint.tvCaps[cn]!=NULL) { tvCaps.AppendString(endpoint.tvCaps[cn]); cn++; }

 //cout << tsCaps << "\n";
 //cout << tvCaps << "\n";

 scap = -1; vcap = -1;
 for(int cn=0; cn<par.GetSize() && (scap < 0 || vcap < 0); cn++)
 {
  SipCapMapType::iterator cir = caps.find(par[cn]);
  SipCapability &c = cir->second[0];
  cout << c.format << "\n";
  if(c.media == 0)
  {
   if(scap >= 0) continue;
   // PCMU
   if(c.format.ToLower() == "pcmu" && tsCaps.GetStringsIndex("G.711-uLaw-64k")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "G.711-uLaw-64k{sw}"; c.cap = H323Capability::Create("G.711-uLaw-64k{sw}"); }
   // PCMA
   else if(c.format.ToLower() == "pcma" && tsCaps.GetStringsIndex("G.711-ALaw-64k")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "G.711-ALaw-64k{sw}"; c.cap = H323Capability::Create("G.711-ALaw-64k{sw}"); }
   // G.722
   else if(c.format.ToLower() == "g722" && tsCaps.GetStringsIndex("G.722-64k{sw}")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "G.722-64k{sw}"; c.cap = H323Capability::Create("G.722-64k{sw}"); }
   // G.723.1
   else if(c.format.ToLower() == "g723" && tsCaps.GetStringsIndex("G.7231-6.3k[e]{sw}")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "G.7231-6.3k[e]{sw}"; c.cap = H323Capability::Create("G.7231-6.3k[e]{sw}"); }
   // G.728
   else if(c.format.ToLower() == "g728" && tsCaps.GetStringsIndex("G.728-16k[e]")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "G.728-16k[e]"; c.cap = H323Capability::Create("G.728-16k[e]"); }
   // G.729A
   else if(c.format.ToLower() == "g729" && tsCaps.GetStringsIndex("G.729A-8k[e]{sw}")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "G.729A-8k[e]{sw}"; c.cap = H323Capability::Create("G.729A-8k[e]{sw}"); }
   // iLBC-13k3
   else if(c.format.ToLower() == "ilbc" && c.parm == "mode=30;" && tsCaps.GetStringsIndex("iLBC-13k3{sw}")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "iLBC-13k3{sw}"; c.cap = H323Capability::Create("iLBC-13k3{sw}"); }
   // iLBC-15k2
   else if(c.format.ToLower() == "ilbc" && c.parm == "mode=20;" && tsCaps.GetStringsIndex("iLBC-15k2{sw}")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "iLBC-15k2{sw}"; c.cap = H323Capability::Create("iLBC-15k2{sw}"); }
   // SILK 16000
   else if(c.format.ToLower() == "silk" && c.clock == 16000 && tsCaps.GetStringsIndex("SILK_B40{sw}")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "SILK_B40{sw}"; c.cap = H323Capability::Create("SILK_B40{sw}"); }
   // SILK 24000
   else if(c.format.ToLower() == "silk" && c.clock == 24000 && tsCaps.GetStringsIndex("SILK_B40_24K{sw}")!=P_MAX_INDEX)
    { scap = c.payload; c.h323 = "SILK_B40_24K{sw}"; c.cap = H323Capability::Create("SILK_B40_24K{sw}"); }
   // SPEEX
   else if(c.format.ToLower() == "speex") SelectCapability_SPEEX(c,tsCaps);
   // OPUS
   else if(c.format.ToLower() == "opus") SelectCapability_OPUS(c,tsCaps);
  }
  else if(c.media == 1)
  {
   if(vcap >= 0) continue;
   if(c.format.ToLower() == "h263") SelectCapability_H263(c,tvCaps);
   else if(c.format.ToLower() == "h263-1998") SelectCapability_H263p(c,tvCaps);
   else if(c.format.ToLower() == "h264") SelectCapability_H264(c,tvCaps);
   else if(c.format.ToLower() == "vp8") SelectCapability_VP8(c,tvCaps);
  }
 }

 cout << scap << " " << vcap << "\r\n";
 if(scap < 0 && vcap < 0)
   return 0;

 sdp_msg = "v=0\r\no=";
 sdp_msg = sdp_msg + requestedRoom + " ";
 sdp_seq++;
 sdp_msg = sdp_msg + PString(sdp_id) + " ";
 sdp_msg = sdp_msg + PString(sdp_seq);
 sdp_msg = sdp_msg + " IN IP4 ";
 sdp_msg = sdp_msg + localIP + "\r\n";
 sdp_msg = sdp_msg + "s=openmcu\r\n";
 sdp_msg = sdp_msg + "c=IN IP4 ";
 sdp_msg = sdp_msg + localIP + "\r\n";
 if(bandwidth) sdp_msg = sdp_msg + "b=AS:" + PString(bandwidth) + "\r\n";
 sdp_msg = sdp_msg + "t=0 0\r\n";
 return 1;
}

int OpenMCUSipConnection::ProcessInviteEvent()
{
 PTRACE(1, "MCUSIP\tProcessInviteEvent");
 su_home_t *home = msg_home(c_sip_msg);
 sip_t *sip = sip_object(c_sip_msg);

 sdp_s = sip->sip_payload->pl_data;
 PStringArray sdp_sa = sdp_s.Lines();

 requestedRoom = roomName;

 sip_addr_t *remote_addr_t;
 if(direction == 0) remote_addr_t = sip_from_dup(home, sip->sip_from);
 else remote_addr_t = sip_to_dup(home, sip->sip_to);

 if(sip->sip_contact && sip->sip_contact->m_display && strcmp(sip->sip_contact->m_display, "") != 0)
   remotePartyName = sip->sip_contact->m_display;
 else if(remote_addr_t->a_display && strcmp(remote_addr_t->a_display, "") != 0)
   remotePartyName = remote_addr_t->a_display;
 else
   remotePartyName = remote_addr_t->a_url->url_user;

 remotePartyName.Replace("\"","",TRUE,0);
 remotePartyName = PURL::UntranslateString(remotePartyName, PURL::QueryTranslation);

 remoteName = remote_addr_t->a_url->url_user;
 remoteName = PURL::UntranslateString(remoteName, PURL::QueryTranslation);

 remotePartyAddress = PString("sip:")+remoteName+"@"+remote_addr_t->a_url->url_host;

 if(sip->sip_user_agent && sip->sip_user_agent->g_string)
  remoteApplication = sip->sip_user_agent->g_string;

 callToken = remotePartyName + "@" + remotePartyAddress + ":" + PString(sip->sip_call_id->i_id);
 cout << "Name: " << remotePartyName << " Addr: " << remotePartyAddress << "\n";

 if(!ProcessSDP(sdp_sa, sipCapsId, sipCaps, 0))
   return 415;

 ep.OnIncomingSipConnection(callToken,*this);
 PTRACE(1, "MCUSIP\tJoinConference");
 JoinConference(requestedRoom);
 if(conferenceMember == NULL || conference == NULL)
   return 600;
 PTRACE(1, "MCUSIP\tCreateLogicalChannels");
 CreateLogicalChannels();
 return 1;
}

int OpenMCUSipConnection::ProcessReInviteEvent()
{
 PTRACE(1, "MCUSIP\tProcessReInviteEvent");
 sip_t *sip = sip_object(c_sip_msg);

 PString sdp = sip->sip_payload->pl_data;
 PStringArray sdp_sa = sdp.Lines();
 PIntArray new_par;
 SipCapMapType new_caps;
 
 int cur_scap = scap;
 int cur_vcap = vcap;
 PString cur_sdp_msg = sdp_msg;

 if(!ProcessSDP(sdp_sa, new_par, new_caps, 1))
   return 415;
 
 int sflag = 1; // 0 - no changes
 cout << "Scap: " << scap << " Cur_Scap: " << cur_scap << "\n";
 if(scap >= 0 && cur_scap >= 0)
 {
  SipCapMapType::iterator cir = sipCaps.find(cur_scap);
  SipCapability *cur_sc = cir->second;
  cir = new_caps.find(scap);
  SipCapability *new_sc = cir->second;
  sflag = new_sc->CmpSipCaps(*cur_sc);
  if(!sflag) sdp_msg += new_sc->sdp;
 }
 else if(scap < 0 && cur_scap < 0) sflag = 0;
 if(sflag && cur_scap>=0)
 {
  StopChannel(cur_scap,1);
  StopChannel(cur_scap,0);
  DeleteMediaChannels(cur_scap);
 }

 int vflag = 1; // 0 - no changes
 if(vcap >= 0 && cur_vcap >= 0)
 {
  SipCapMapType::iterator cir = sipCaps.find(cur_vcap);
  SipCapability *cur_sc = cir->second;
  cir = new_caps.find(vcap);
  SipCapability *new_sc = cir->second;
  vflag = new_sc->CmpSipCaps(*cur_sc);
  if(!vflag) sdp_msg += new_sc->sdp;
 }
 else if(vcap < 0 && cur_vcap < 0) vflag = 0;
 if(vflag && cur_vcap>=0)
 {
  StopChannel(cur_vcap,1);
  StopChannel(cur_vcap,0);
  DeleteMediaChannels(cur_vcap);
 }
 
 if(!sflag && !vflag) // nothing changed
 {
  // sending old sdp
  return 1;
 }

 sipCapsId.SetSize(0);
 sipCaps.clear();
 sipCapsId = new_par;
 sipCaps = new_caps;
 
 if(sflag && scap>=0)
 {
  CreateAudioChannel(scap,0);
  CreateAudioChannel(scap,1);
 }
 
 if(vflag && vcap>=0)
 {
  CreateVideoChannel(vcap,0);
  CreateVideoChannel(vcap,1);
 }
 return 1;
}

int OpenMCUSipConnection::SendBYE(nta_agent_t *agent)
{
  PTRACE(1, "MCUSIP\tSendBYE");
  sip_t *sip = sip_object(c_sip_msg);
  su_home_t *home = msg_home(c_sip_msg);

  // Send BYE
  sip_addr_t *sip_from, *sip_to;
  if(direction == 0)
  {
    sip_from = sip_to_dup(home, sip->sip_to);
    sip_to = sip_from_dup(home, sip->sip_from);
  } else {
    sip_from = sip_from_dup(home, sip->sip_from);
    sip_to = sip_to_dup(home, sip->sip_to);
  }

  sip_request_t *sip_rq = sip_request_create(home, SIP_METHOD_BYE, (url_string_t *)sip_to->a_url, NULL);
  sip_cseq_t *sip_cseq = sip_cseq_create(home, sip->sip_cseq->cs_seq, SIP_METHOD_BYE);
  sip_route_t* sip_route = sip_route_reverse(home, sip->sip_record_route);
  msg_t *sip_msg = nta_msg_create(agent, 0);
  nta_outgoing_t *a_orq = nta_outgoing_mcreate(agent, NULL, NULL,
			(url_string_t *)sip_to->a_url,
			sip_msg,
 			SIPTAG_REQUEST(sip_rq),
			SIPTAG_ROUTE(sip_route),
                        SIPTAG_CONTACT(contact_t),
			SIPTAG_FROM(sip_from),
			SIPTAG_TO(sip_to),
			SIPTAG_CSEQ(sip_cseq),
			SIPTAG_CALL_ID(sip->sip_call_id),
			SIPTAG_USER_AGENT_STR((const char*)(MCUSIP_USER_AGENT_STR)),
			TAG_END());
  if(a_orq == NULL)
    return 0;
  nta_outgoing_destroy(a_orq);

  PTRACE(1, "MCUSIP\tSend SIP message: \n" << msg_as_string(home, sip_msg, NULL, 0, NULL));
  return 1;
}

int OpenMCUSipEndPoint::ProcessH323toSipQueue(const SipKey &key, OpenMCUSipConnection *sCon)
{
 PString *cmd = sCon->cmdQueue.Pop();
 while(cmd != NULL)
 {
  if(*cmd == "BYE")
  {
   delete cmd;
   sCon->SendBYE(agent);
   sCon->StopTransmitChannels();
   sCon->StopReceiveChannels();
   sCon->DeleteChannels();
   sCon->LeaveConference(TRUE); // leave conference and delete connection
   cmd = sCon->cmdQueue.Pop() ;
   PTRACE(1, "MCUSIP\tSIP BYE sent\n");
   return 1;
  }
  cmd = sCon->cmdQueue.Pop();
 }
 return 0;
}

PString OpenMCUSipEndPoint::GetRoomAccess(const sip_t *sip)
{
    PTRACE(1, "MCUSIP\tGetRoomAccess");
    BOOL inRoom = false;
    PString via = sip->sip_via->v_host;
    PString userName = sip->sip_from->a_url->url_user;
    PString hostName = sip->sip_from->a_url->url_host;
    PString roomName;
    PString userName_, hostName_, via_, access;
    PString defaultAccess = MCUConfig("RoomAccess").GetString("*", "ALLOW").ToUpper();

    if(sip->sip_record_route)
    {
      ProxyServerMapType::iterator it =
        ProxyServerMap.find((PString)sip->sip_to->a_url->url_user+"@"+(PString)sip->sip_to->a_url->url_host);
      if(it == ProxyServerMap.end())
        return 0;
      roomName = it->second->roomName;
    }
    else
      roomName = sip->sip_to->a_url->url_user;

    PStringToString data = MCUConfig("RoomAccess").GetAllKeyValues();

    if(roomName != "*")
      access = data(roomName).Tokenise(" ")[0].ToUpper();

    PStringArray accessList = data(roomName).Tokenise(" ")[1].Tokenise(",");
    for(int i=0; accessList[i] != NULL; i++)
    {
      userName_ = accessList[i].Tokenise("@")[0];
      hostName_ = accessList[i].Tokenise("@")[1];
      via_ = accessList[i].Tokenise("@")[2];
      if((userName_ == "" && hostName_ == "" && via_ == via) ||
         (userName_ == "" && hostName_ == hostName && via_ == "") ||
         (userName_ == "" && hostName_ == hostName && via_ == via) ||
         (userName_ == userName && hostName_ == "" && via_ == "") ||
         (userName_ == userName && hostName_ == "" && via_ == via) ||
         (userName_ == userName && hostName_ == hostName && via_ == "") ||
         (userName_ == userName && hostName_ == hostName && via_ == via)
        )
      {
        inRoom = true;
        break;
      }
    }

    if(inRoom && access == "ALLOW")
      access = "ALLOW";
    else if(inRoom && access == "DENY")
      access = "DENY";
    else if(inRoom == FALSE && access == "ALLOW")
      access = "DENY";
    else if(inRoom == FALSE && access == "DENY")
      access = "ALLOW";
    else
      access = defaultAccess;

    PTRACE(1, "MCUSIP\t"<< access << " access to room \"" << roomName << "\", from=" << userName+"@"+hostName << ", via=" << via);
    return access;
}

void OpenMCUSipEndPoint::SipMakeCall(PString room, PString to)
{
    PTRACE(1, "MCUSIP\tSipMakeCall");
    if(agent == NULL)
      return;

    PString localIP, remoteIP, remotePort, proxyIP, userName, roomName;
    BOOL needProxy = false;

    PString addr = to.Tokenise(";")[0];
    remoteIP = addr.Tokenise(":")[1].Tokenise("@")[1];
    if(remoteIP == "")
      return;
    remotePort = addr.Tokenise(":")[2];
    if(remotePort == "")
      remotePort = "5060";

    ProxyServerMapType::iterator it;
    for(it=ProxyServerMap.begin(); it!=ProxyServerMap.end(); it++)
    {
      ProxyServer *proxy = it->second;
      if(proxy->proxyIP == remoteIP)
        needProxy = true;
      if(proxy->proxyIP == remoteIP && proxy->roomName == room && proxy->enable == 1)
      {
        localIP = proxy->localIP;
        proxyIP = proxy->proxyIP;
        userName = proxy->userName;
        roomName = proxy->roomName;
        break;
      }
    }
    if(roomName == NULL && needProxy)
      return;
    if(roomName == NULL)
    {
      localIP = GetFromIp(remoteIP, remotePort);
      if(localIP == "0")
        return;
      proxyIP=localIP;
      userName = room;
      roomName = room;
    }

    sip_addr_t *sip_from = sip_from_create(&home, (url_string_t *)(const char *)
	("sip:"+userName+"@"+proxyIP));
    sip_from->a_display = roomName;
    sip_from_tag(&home, sip_from, nta_agent_newtag(&home, "tag=%s", agent));

    sip_addr_t *sip_to = sip_to_create(&home, (url_string_t *)(const char *)to);

    sip_contact_t *sip_contact = sip_contact_create(&home, (url_string_t *)(const char *)
	("sip:"+userName+"@"+localIP+":"+localPort), NULL);
    sip_contact->m_display = roomName;

    // finding the RTP ports
    unsigned aPort, vPort;
    aPort = GetLocalDataPort(localIP, ep->GetRtpIpPortBase(), ep->GetRtpIpPortMax());
    vPort = GetLocalDataPort(localIP, aPort+2, ep->GetRtpIpPortMax());
    if(aPort == 0 || vPort == 0) return;
    // add ports to call_id string
    PString call_id_suffix = "0@"+PString(aPort)+"@"+PString(vPort);

    // create sdp for outgoing request
    sdpInvite = CreateSdpInvite();
    PString sdp = sdpInvite;
    sdp.Replace("USERNAME", room, TRUE, 0);
    sdp.Replace("LOCALIP", localIP, TRUE, 0);
    sdp.Replace("RTP_AUDIO_PORT", aPort, TRUE, 0);
    sdp.Replace("RTP_VIDEO_PORT", vPort, TRUE, 0);
    sip_payload_t *sip_payload = sip_payload_format(&home, sdp);

    sip_request_t *sip_rq = sip_request_create(&home, SIP_METHOD_INVITE, (url_string_t *)sip_to->a_url, NULL);
    sip_cseq_t *sip_cseq = sip_cseq_create(&home, (rand()%1000000), SIP_METHOD_INVITE);
    sip_call_id_t* sip_call_id = sip_call_id_create(&home, (const char*)call_id_suffix);

    msg_t *sip_msg = nta_msg_create(agent, 0);
    nta_outgoing_t *orq = nta_outgoing_mcreate(agent, ProcessSipEventWrap_ntaout, (nta_outgoing_magic_t *)this,
			(url_string_t *)sip_to->a_url,
			sip_msg,
			SIPTAG_REQUEST(sip_rq),
			SIPTAG_FROM(sip_from),
			SIPTAG_TO(sip_to),
			SIPTAG_CSEQ(sip_cseq),
			SIPTAG_CALL_ID(sip_call_id),
			SIPTAG_CONTACT(sip_contact),
			SIPTAG_PAYLOAD(sip_payload),
			SIPTAG_CONTENT_TYPE_STR("application/sdp"),
			SIPTAG_USER_AGENT_STR((const char*)(MCUSIP_USER_AGENT_STR)),
			TAG_END());
    // su_epoll_port.c:206: su_epoll_port_register: Assertion `su_port_own_thread(self)' failed.
    // without registration only send invite to IP address, do not use domain name
    if(orq == NULL)
      return;
    PTRACE(1, "MCUSIP\tSend SIP message: \n" << msg_as_string(&home, sip_msg, NULL, 0, NULL));
}

void OpenMCUSipEndPoint::SipRegister(ProxyServer *proxy)
{
    PTRACE(1, "MCUSIP\tSipRegister");
    if(agent == NULL)
      return;

    sip_addr_t *sip_from = sip_from_create(&home, (url_string_t *)(const char *)
	("sip:"+proxy->userName+"@"+proxy->proxyIP));
    sip_from_tag(&home, sip_from, nta_agent_newtag(&home, "tag=%s", agent));
    sip_from->a_display = proxy->roomName;

    sip_addr_t *sip_to = sip_to_create(&home, (url_string_t *)(const char *)
	("sip:"+proxy->userName+"@"+proxy->proxyIP+":"+proxy->proxyPort));

    sip_contact_t *sip_contact = sip_contact_create(&home, (url_string_t *)(const char *)
	("sip:"+proxy->userName+"@"+proxy->localIP+":"+proxy->localPort), NULL);
    sip_contact->m_display = proxy->roomName;

    sip_request_t *sip_rq = sip_request_create(&home, SIP_METHOD_REGISTER, (url_string_t *)sip_to->a_url, NULL);
    sip_cseq_t *sip_cseq = sip_cseq_create(&home, (rand()%1000000), SIP_METHOD_REGISTER);
    sip_call_id_t* sip_call_id = sip_call_id_create(&home, "0");
    msg_t *sip_msg = nta_msg_create(agent, 0);
    nta_outgoing_t *orq = nta_outgoing_mcreate(agent, ProcessSipEventWrap_ntaout, (nta_outgoing_magic_t *)this,
      			(url_string_t *)sip_to->a_url,
			sip_msg,
			SIPTAG_REQUEST(sip_rq),
			SIPTAG_FROM(sip_from),
			SIPTAG_TO(sip_to),
			SIPTAG_CSEQ(sip_cseq),
			SIPTAG_CALL_ID(sip_call_id),
			SIPTAG_CONTACT(sip_contact),
			SIPTAG_EXPIRES_STR((const char*)proxy->expires),
			SIPTAG_ALLOW_EVENTS_STR("INVITE, ACK, BYE"),
			SIPTAG_USER_AGENT_STR((const char*)(MCUSIP_USER_AGENT_STR)),
			TAG_END());
    if(orq == NULL)
      return;
    PTRACE(1, "MCUSIP\tSend SIP message: \n" << msg_as_string(&home, sip_msg, NULL, 0, NULL));
}

PString OpenMCUSipEndPoint::MakeAuthStr(ProxyServer *proxy, const sip_t *sip)
{
    PTRACE(1, "MCUSIP\tMakeAuthStr");
    const char *scheme=NULL, *realm=NULL, *nonce=NULL, *sip_auth_str=NULL, *method=NULL;
    if(sip->sip_www_authenticate && sip->sip_www_authenticate->au_scheme && sip->sip_www_authenticate->au_params)
    {
      realm = msg_params_find(sip->sip_www_authenticate->au_params, "realm=");
      nonce = msg_params_find(sip->sip_www_authenticate->au_params, "nonce=");
      scheme = sip->sip_www_authenticate->au_scheme;
    }
    else if(sip->sip_proxy_authenticate && sip->sip_proxy_authenticate->au_scheme && sip->sip_proxy_authenticate->au_params)
    {
      realm = msg_params_find(sip->sip_proxy_authenticate->au_params, "realm=");
      nonce = msg_params_find(sip->sip_proxy_authenticate->au_params, "nonce=");
      scheme = sip->sip_proxy_authenticate->au_scheme;
    }
    else
      return NULL;

    method = sip->sip_cseq->cs_method_name;

    PString uri = "sip:"+proxy->userName+"@"+realm; uri.Replace("\"","",true,0);
    auth_response_t ar[1] = {{ sizeof(ar) }};
    auth_hexmd5_t ha1, hresponse;
    ar->ar_username = proxy->userName;
    ar->ar_realm = realm;
    ar->ar_nonce = nonce;
    ar->ar_uri = uri;
    //auth_digest_ha1(ha1, proxy->userName, realm, proxy->password);
    auth_digest_a1(ar, ha1, proxy->password);
    auth_digest_response(ar, hresponse, ha1, method, NULL, 0);
    sip_auth_str = su_sprintf(&home, "%s %s\"%s\", %s%s, %s%s, %s\"%s\", %s\"%s\", %s",
			    scheme,
			    "username=", ar->ar_username,
			    "realm=", realm,
			    "nonce=", nonce,
			    "response=", hresponse,
			    "uri=", ar->ar_uri,
			    "algorithm=MD5");
    return sip_auth_str;
}

int OpenMCUSipEndPoint::ProcessSipEvent_ntaout(nta_outgoing_magic_t *context, nta_outgoing_t *orq, const sip_t *sip)
{
  msg_t *msg = nta_outgoing_getresponse(orq);
  if(msg == NULL || sip->sip_status == NULL ||
     sip->sip_call_id == NULL || sip->sip_to->a_url->url_host == NULL)
    return 0;

  PString sip_msg_str = (PString)msg_as_string(&home, msg, NULL, 0, NULL);
  PTRACE(1, "MCUSIP\tReceived SIP message: \n" << sip_msg_str);

  SipKey sik;
  sik.addr = inet_addr(sip->sip_to->a_url->url_host);
  if(sip->sip_to->a_url->url_port) sik.port = atoi(sip->sip_to->a_url->url_port); else sik.port=5060;
  sik.sid = sip->sip_call_id->i_id;

  unsigned status = sip->sip_status->st_status;

  if((status == 401 || status == 407) && sip->sip_cseq &&
    (sip->sip_cseq->cs_method == sip_method_register || sip->sip_cseq->cs_method == sip_method_invite))
  {
    // the number of unauthorized requests for register/invite
    unsigned reqNum= atoi(PString(sip->sip_call_id->i_id).Tokenise("@")[1]);
    if(reqNum > 2)
    {
      nta_outgoing_destroy(orq);
      return 0;
    }
    PString call_id_suffix = PString(reqNum+1);

    ProxyServerMapType::iterator it = ProxyServerMap.find((PString)sip->sip_from->a_url->url_user+"@"+(PString)sip->sip_to->a_url->url_host);
    if(it == ProxyServerMap.end())
      return 0;
    ProxyServer *proxy = it->second;

    sip_addr_t *sip_from = sip_from_create(&home, (url_string_t *)(const char *)
	("sip:"+proxy->userName+"@"+proxy->proxyIP));
    sip_from->a_display = proxy->roomName;
    sip_from_tag(&home, sip_from, nta_agent_newtag(&home, "tag=%s", agent));

    sip_addr_t *sip_to = sip_to_create(&home, (url_string_t *)sip->sip_to->a_url);

    sip_contact_t *sip_contact = sip_contact_create(&home, (url_string_t *)(const char *)
	("sip:"+proxy->userName+"@"+proxy->localIP+":"+proxy->localPort), NULL);
    sip_contact->m_display = proxy->roomName;

    sip_payload_t *sip_payload=NULL;
    if(sip->sip_cseq->cs_method == sip_method_invite)
    {
      // finding the RTP ports
      unsigned aPort, vPort;
      aPort = GetLocalDataPort(proxy->localIP, ep->GetRtpIpPortBase(), ep->GetRtpIpPortMax());
      vPort = GetLocalDataPort(proxy->localIP, aPort+2, ep->GetRtpIpPortMax());
      if(aPort == 0 || vPort == 0) return 0;
      // add ports to call_id string
      call_id_suffix += "@"+PString(aPort)+"@"+PString(vPort);

      PString sdp = sdpInvite;
      sdp.Replace("USERNAME", proxy->userName, TRUE, 0);
      sdp.Replace("LOCALIP", proxy->localIP, TRUE, 0);
      sdp.Replace("RTP_AUDIO_PORT", aPort, TRUE, 0);
      sdp.Replace("RTP_VIDEO_PORT", vPort, TRUE, 0);
      sip_payload = sip_payload_format(&home, sdp);
    }

    sip_request_t *sip_rq = sip_request_create(&home, sip->sip_cseq->cs_method,
			sip->sip_cseq->cs_method_name, (url_string_t *)sip_to->a_url, NULL);
    sip_cseq_t *sip_cseq = sip_cseq_create(&home, (rand()%1000000),
			sip->sip_cseq->cs_method, sip->sip_cseq->cs_method_name);
    sip_call_id_t* sip_call_id = sip_call_id_create(&home, (const char *)call_id_suffix);

    proxy->sipAuthStr = MakeAuthStr(proxy, sip);
    sip_authorization_t *sip_auth=NULL, *sip_proxy_auth=NULL;
    if(status == 401)
      sip_auth = sip_authorization_make(&home, proxy->sipAuthStr);
    else if(status == 407)
      sip_proxy_auth = sip_proxy_authorization_make(&home, proxy->sipAuthStr);

    nta_response_f *callback = NULL;
    if(sip->sip_cseq->cs_method == sip_method_invite)
       callback = ProcessSipEventWrap_ntaout;

    msg_t *sip_msg = nta_msg_create(agent, 0);
    nta_outgoing_t *a_orq = nta_outgoing_mcreate(agent, callback, (nta_outgoing_magic_t *)this,
			(url_string_t *)sip->sip_to->a_url,
			sip_msg,
			SIPTAG_REQUEST(sip_rq),
			SIPTAG_FROM(sip_from),
			SIPTAG_TO(sip_to),
			SIPTAG_CSEQ(sip_cseq),
			SIPTAG_CALL_ID(sip_call_id),
			SIPTAG_CONTACT(sip_contact),
			SIPTAG_AUTHORIZATION(sip_auth),
			SIPTAG_PROXY_AUTHORIZATION(sip_proxy_auth),
			SIPTAG_PAYLOAD(sip_payload),
			SIPTAG_CONTENT_TYPE_STR("application/sdp"),
			SIPTAG_ALLOW_EVENTS_STR("INVITE, ACK, BYE"),
			SIPTAG_USER_AGENT_STR((const char*)(MCUSIP_USER_AGENT_STR)),
			TAG_END());
    if(a_orq == NULL)
      return 0;
    PTRACE(1, "MCUSIP\tSend SIP message: \n" << msg_as_string(&home, sip_msg, NULL, 0, NULL));

    nta_outgoing_destroy(orq);
    return 0;
  }

  if(status == 200)
  {
    if(sip->sip_payload==NULL) return 0;
    if(sip->sip_payload->pl_data==NULL) return 0;

    // Send ACK
    sip_request_t *sip_rq;
    if(sip->sip_contact && sip->sip_contact->m_url)
      sip_rq = sip_request_create(&home, SIP_METHOD_ACK, (url_string_t *)sip->sip_contact->m_url, NULL);
    else
      sip_rq = sip_request_create(&home, SIP_METHOD_ACK, (url_string_t *)sip->sip_to->a_url, NULL);
    sip_cseq_t *sip_cseq = sip_cseq_create(&home, sip->sip_cseq->cs_seq, SIP_METHOD_ACK);
    sip_route_t* sip_route = sip_route_reverse(&home, sip->sip_record_route);
    msg_t *sip_msg = nta_msg_create(agent, 0);
    nta_outgoing_t *a_orq = nta_outgoing_mcreate(agent, NULL, NULL,
			(url_string_t *)sip->sip_to->a_url,
			sip_msg,
			SIPTAG_REQUEST(sip_rq),
			SIPTAG_ROUTE(sip_route),
			SIPTAG_FROM(sip->sip_from),
			SIPTAG_TO(sip->sip_to),
			SIPTAG_CSEQ(sip_cseq),
			SIPTAG_CALL_ID_STR((const char*)sik.sid),
			SIPTAG_USER_AGENT_STR((const char*)(MCUSIP_USER_AGENT_STR)),
			TAG_END());
    if(a_orq == NULL)
      return 0;
    nta_outgoing_destroy(a_orq);

    PTRACE(1, "MCUSIP\tSend SIP message: \n" << msg_as_string(&home, sip_msg, NULL, 0, NULL));

    SipConnectionMapType::iterator scr = sipConnMap.find(sik);
    if(scr != sipConnMap.end())  // connection already exist
      return 0;

    OpenMCUSipConnection *sCon = new OpenMCUSipConnection(this, ep);
    sCon->direction = 1;
    sCon->c_sip_msg = msg_dup(msg);

    if(CreateConData(sCon) != 1)
      return 0;

    if(sCon->ProcessInviteEvent() != 1)
      return 0;

    sCon->StartReceiveChannels();
    sipConnMap.insert(SipConnectionMapType::value_type(sik,sCon));
    sCon->StartTransmitChannels();

    nta_outgoing_destroy(orq);
    return 0;
  }

  if(status > 200)
    nta_outgoing_destroy(orq);

  return 0;
}

int OpenMCUSipEndPoint::CreateConData(OpenMCUSipConnection *sCon)
{
  PTRACE(1, "MCUSIP\tCreateConData");
  sip_t *sip = sip_object(sCon->c_sip_msg);

  if(sCon->direction == 0) //incoming
  {
    if(sip->sip_record_route)
    {
      ProxyServerMapType::iterator it =
          ProxyServerMap.find((PString)sip->sip_to->a_url->url_user+"@"+(PString)sip->sip_to->a_url->url_host);
      if(it == ProxyServerMap.end())
        return 0;
      ProxyServer *proxy = it->second;
      sCon->contact_t = sip_contact_create(&home, (url_string_t *)(const char *)
	  ("sip:"+proxy->userName+"@"+proxy->localIP+":"+proxy->localPort), NULL);
      sCon->localIP = proxy->localIP;
      sCon->roomName = proxy->roomName;
    } else {
      sCon->contact_t = sip_contact_create(&home, (url_string_t *)(const char *)
	  ("sip:"+(PString)sip->sip_to->a_url->url_user+"@"+(PString)sip->sip_to->a_url->url_host+":"+localPort), NULL);
      sCon->localIP = sip->sip_to->a_url->url_host;
      sCon->roomName = sip->sip_to->a_url->url_user;
    }
    sCon->audioRtpPort = 0;
    sCon->videoRtpPort = 0;
  } else { // outgoing
    if(sip->sip_record_route)
    {
      ProxyServerMapType::iterator it =
          ProxyServerMap.find((PString)sip->sip_from->a_url->url_user+"@"+(PString)sip->sip_from->a_url->url_host);
      if(it == ProxyServerMap.end())
        return 0;
      ProxyServer *proxy = it->second;
      sCon->contact_t = sip_contact_create(&home, (url_string_t *)(const char *)
	  ("sip:"+proxy->userName+"@"+proxy->localIP+":"+proxy->localPort), NULL);
      sCon->localIP = proxy->localIP;
      sCon->roomName = proxy->roomName;
    } else {
      sCon->contact_t = sip_contact_create(&home, (url_string_t *)(const char *)
	  ("sip:"+(PString)sip->sip_from->a_url->url_user+"@"+(PString)sip->sip_from->a_url->url_host+":"+localPort), NULL);
      sCon->localIP = sip->sip_from->a_url->url_host;
      sCon->roomName = sip->sip_from->a_url->url_user;
    }
    sCon->audioRtpPort = atoi(PString(sip->sip_call_id->i_id).Tokenise("@")[2]);
    sCon->videoRtpPort = atoi(PString(sip->sip_call_id->i_id).Tokenise("@")[3]);
    if(sCon->audioRtpPort == 0 || sCon->videoRtpPort == 0)
      return 0;
  }
  return 1;
}

int OpenMCUSipEndPoint::ReqReply(msg_t *msg, unsigned method, const char *method_name=NULL, OpenMCUSipConnection *sCon=NULL)
{
  PTRACE(1, "MCUSIP\tReqReply");
  sip_t *sip = sip_object(msg);
  if(sip == NULL)
    return 0;

  const char *status_phrase = NULL;
  status_phrase = sip_status_phrase(method);
  if(status_phrase == NULL)
    return 0;

  PTRACE(1, "MCUSIP\tSending " << method_name << " to " <<
    sip->sip_from->a_url->url_user << "@" << sip->sip_from->a_url->url_host);

  sip_payload_t *sip_payload = NULL;
  if(sCon != NULL)
  {
    sip_payload = sip_payload_format(&home, (const char *)sCon->sdp_msg);
    PTRACE(1, "MCUSIP\tsdp_msg\n" << sCon->sdp_msg);
  }

  if(method == 200 && sip_payload != NULL)
    nta_msg_treply(agent, msg, method, status_phrase,
                   SIPTAG_CONTACT(sCon->contact_t),
                   SIPTAG_CONTENT_TYPE_STR("application/sdp"),
                   SIPTAG_PAYLOAD(sip_payload),
                   SIPTAG_USER_AGENT_STR((const char*)(MCUSIP_USER_AGENT_STR)),
                   TAG_END());
  else
    nta_msg_treply(agent, msg, method, status_phrase,
                   SIPTAG_USER_AGENT_STR((const char*)(MCUSIP_USER_AGENT_STR)),
                   TAG_END());
  return 0;
}

int OpenMCUSipEndPoint::ProcessSipEvent_cb(nta_agent_t *agent, msg_t *msg, sip_t *sip)
{
  if(msg==NULL || sip->sip_request==NULL || sip->sip_request->rq_method_name==NULL ||
     sip->sip_from->a_url->url_host==NULL || sip->sip_call_id==NULL)
    return ReqReply(msg, SIP_400_BAD_REQUEST);

  PString sip_msg_str = (PString)msg_as_string(&home, msg, NULL, 0, NULL);
  PTRACE(1, "MCUSIP\tReceived SIP message: \n" << sip_msg_str);

  SipKey sik;
  sik.addr = inet_addr(sip->sip_from->a_url->url_host);
  sik.sid = sip->sip_call_id->i_id;
  if(sip->sip_from->a_url->url_port) sik.port = atoi(sip->sip_from->a_url->url_port); else sik.port=5060;

  PString request = sip->sip_request->rq_method_name;

  if(request == "INVITE")
  {
    if(GetRoomAccess(sip) == "DENY")
      return ReqReply(msg, SIP_403_FORBIDDEN);

    if(sip->sip_payload==NULL || (sip->sip_payload!=NULL && sip->sip_payload->pl_data==NULL))
      return ReqReply(msg, SIP_415_UNSUPPORTED_MEDIA);

   SipConnectionMapType::iterator scr = sipConnMap.find(sik);
   if(scr != sipConnMap.end())  // connection already exist, process reinvite
   {
     OpenMCUSipConnection *sCon = scr->second;
     sCon->direction = 0;
     sCon->c_sip_msg = msg_dup(msg);
     if(MCUConfig("Parameters").GetBoolean(SIPReInviteKey, TRUE))
     {
       int ret = sCon->ProcessReInviteEvent();
       if(ret != 1)
         return ReqReply(msg, ret);
       ReqReply(msg, SIP_200_OK, sCon);
       sCon->StartReceiveChannels(); // start receive logical channels
     } else {
       sipConnMap.erase(sik);
       ReqReply(msg, SIP_405_METHOD_NOT_ALLOWED);
       sCon->LeaveConference(TRUE);
     }
     return 0;
   }

   PTRACE(1, "MCUSIP\tNew SIP INVITE");
   OpenMCUSipConnection *sCon = new OpenMCUSipConnection(this, ep);
   sCon->direction = 0;
   sCon->c_sip_msg = msg_dup(msg);

   if(CreateConData(sCon) != 1)
     return ReqReply(msg, SIP_500_INTERNAL_SERVER_ERROR);

   int ret = sCon->ProcessInviteEvent();
   if(ret != 1)
     return ReqReply(msg, ret);

   ReqReply(msg, SIP_200_OK, sCon);
   sCon->StartReceiveChannels(); // start receive logical channels
   sipConnMap.insert(SipConnectionMapType::value_type(sik,sCon));
   return 0;
  }
  if(request == "ACK")
  {
    SipConnectionMapType::iterator scr = sipConnMap.find(sik);
    if(scr == sipConnMap.end())
      return 0;

    PTRACE(1, "MCUSIP\tNew SIP ACK accepted");
    OpenMCUSipConnection *sCon = scr->second;
    sip_t *c_sip = sip_object(sCon->c_sip_msg); // replace to_tag
    msg_header_insert(sCon->c_sip_msg, (msg_pub_t *)c_sip, (msg_header_t *)sip->sip_to);
    sCon->StartTransmitChannels(); // start transmit logical channels
    return 0;
  }
  if(request == "BYE")
  {
    SipConnectionMapType::iterator scr = sipConnMap.find(sik);
    if(scr == sipConnMap.end())
    {
      PTRACE(1, "MCUSIP\tSend 200 OK for BYE, connection not found");
      return ReqReply(msg, SIP_200_OK);
    }
    PTRACE(1, "MCUSIP\tNew SIP BYE");
    OpenMCUSipConnection *sCon = scr->second;
    sipConnMap.erase(sik);
    ReqReply(msg, SIP_200_OK);
    sCon->LeaveConference(TRUE); // leave conference and delete connection
    return 0;
  }
  if(request == "OPTIONS" || request == "SUBSCRIBE" || request == "INFO" )
  {
    return ReqReply(msg, SIP_200_OK);
  }

  return 0;
}

void OpenMCUSipEndPoint::MainLoop()
{
  SipConnectionMapType::iterator scr;
  while(1)
  {
    if(terminating) return;
    for (scr = sipConnMap.begin(); scr != sipConnMap.end(); scr++) 
    {
      OpenMCUSipConnection *sCon = scr->second;
      RTP_Session * as = sCon->GetSession(RTP_Session::DefaultAudioSessionID);
      RTP_Session * vs = sCon->GetSession(RTP_Session::DefaultVideoSessionID);
      int count = 0;
      if(as) count += as->GetPacketsReceived() + as->GetRtpcReceived();
      if(vs) count += vs->GetPacketsReceived() + vs->GetRtpcReceived();
      if(count == sCon->inpBytes) sCon->noInpTimeout++;
      else { sCon->noInpTimeout = 0; sCon->inpBytes = count; }
      if(sCon->noInpTimeout == 30) // 15 sec timeout
      {
        PTRACE(1, "MCUSIP\t15 sec timeout waiting incoming stream data");
        sipConnMap.erase(scr->first);
        sCon->StopTransmitChannels();
        sCon->StopReceiveChannels();
        sCon->DeleteChannels();
        sCon->LeaveConference(TRUE); // leave conference and delete connection
        break;
      }
      int bye = ProcessH323toSipQueue(scr->first,sCon);
      if(bye) { SipKey key = scr->first; scr++; sipConnMap.erase(key); if(scr == sipConnMap.end()) break; }
    }
    ProxyServerMapType::iterator it;
    for(it=ProxyServerMap.begin(); it!=ProxyServerMap.end(); it++)
    {
      ProxyServer *proxy = it->second;
      if(proxy->enable == 1 && proxy->timeout == atoi(proxy->expires)*2)
      {
        SipRegister(proxy);
        proxy->timeout = 0;
      }
      proxy->timeout++;
    }
    su_root_sleep(root,500);
    PTRACE(6, "MCUSIP\tSIP Down to sleep");
  }
}

void OpenMCUSipEndPoint::Main()
{
  su_init();
  su_home_init(&home);
  su_log_set_level(NULL, 9);
  root = su_root_create(NULL);

  if(root == NULL) return;

  if(OpenMCU::Current().sipListener!="0.0.0.0")
    agent = nta_agent_create(root, URL_STRING_MAKE((const char*)("sip:"+OpenMCU::Current().sipListener)), ProcessSipEventWrap_cb, (nta_agent_magic_t *)this, NTATAG_UDP_MTU(64000), TAG_NULL());
  else
    agent = nta_agent_create(root, NULL, ProcessSipEventWrap_cb, (nta_agent_magic_t *)this, NTATAG_UDP_MTU(64000), TAG_NULL());

  localPort = (PString)OpenMCU::Current().sipListener.Tokenise(":")[1].Trim();
  if(localPort == "")
    localPort = "5060";

  // proxy servers
  PStringList keys = MCUConfig("ProxyServers").GetKeys();
  for(PINDEX i = 0; i < keys.GetSize(); i++)
  {
    ProxyServer *proxy = new ProxyServer();
    PString tmp = MCUConfig("ProxyServers").GetString(keys[i]);
    proxy->roomName = keys[i];
    proxy->proxyIP = tmp.Tokenise(",")[0].Tokenise(":")[0];
    proxy->proxyPort = tmp.Tokenise(",")[0].Tokenise(":")[1];
    proxy->userName = tmp.Tokenise(",")[1];
    proxy->password = tmp.Tokenise(",")[2];
    proxy->enable = atoi(tmp.Tokenise(",")[3]);
    proxy->expires = tmp.Tokenise(",")[4];
    proxy->timeout = atoi(proxy->expires)*2;
    proxy->localPort = localPort;
    proxy->localIP = GetFromIp((const char *)proxy->proxyIP, (const char *)proxy->proxyPort);
    if(proxy->proxyPort == "") proxy->proxyPort = "5060";
    if(atoi(proxy->expires) < 60) proxy->expires = "60";
    if(atoi(proxy->expires) > 3600) proxy->expires = "3600";
    if(proxy->localIP == "0") continue;
    ProxyServerMap.insert(ProxyServerMapType::value_type(proxy->userName+"@"+proxy->proxyIP, proxy));
  }

  if(agent != NULL)
  {
    MainLoop();
    nta_agent_destroy(agent);
  }

  su_root_destroy(root);
  root = NULL;
  su_home_deinit(&home);
  su_deinit();
}
