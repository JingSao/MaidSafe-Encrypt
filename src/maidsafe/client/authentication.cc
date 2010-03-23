/*
 * copyright maidsafe.net limited 2008
 * The following source code is property of maidsafe.net limited and
 * is not meant for external use. The use of this code is governed
 * by the license file LICENSE.TXT found in the root of this directory and also
 * on www.maidsafe.net.
 *
 * You are not free to copy, amend or otherwise use this source code without
 * explicit written permission of the board of directors of maidsafe.net
 *
 *  Created on: Nov 13, 2008
 *      Author: Team
 */

#include "maidsafe/client/authentication.h"

#include <boost/array.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/thread/mutex.hpp>

#include <vector>

#include "maidsafe/maidsafe.h"
#include "protobuf/datamaps.pb.h"
#include "protobuf/maidsafe_messages.pb.h"
#include "protobuf/maidsafe_service.pb.h"

namespace maidsafe {

char *utils_trim_right(char *szSource) {
  char *pszEOS = 0;
  //  Set pointer to character before terminating NULL
  pszEOS = szSource + strlen(szSource) - 1;
  //  iterate backwards until non '_' is found
  while ((pszEOS >= szSource) && (*pszEOS == ' '))
    --*pszEOS = '\0';
  return szSource;
}

char *utils_trim_left(char *szSource) {
  char *pszBOS = 0;
  //  Set pointer to character before terminating NULL
  // pszEOS = szSource + strlen(szSource) - 1;
  pszBOS = szSource;
  //  iterate backwards until non '_' is found
  while (*pszBOS == ' ')
    ++*pszBOS;
  return pszBOS;
}

char *utils_trim(char *szSource) {
  return utils_trim_left(utils_trim_right(utils_trim_left(szSource)));
}

void Authentication::Init(const boost::uint16_t &max_crypto_thread_count,
                          const boost::uint16_t &crypto_key_buffer_count,
                          boost::shared_ptr<StoreManagerInterface> smgr) {
  sm_ = smgr;
  ss_ = SessionSingleton::getInstance();
  crypto_.set_hash_algorithm(crypto::SHA_512);
  crypto_.set_symm_algorithm(crypto::AES_256);
  crypto_key_pairs_.Init(max_crypto_thread_count, crypto_key_buffer_count);
}

int Authentication::GetUserInfo(const std::string &username,
                                const std::string &pin) {
  user_info_result_ = kPendingResult;
  PacketParams params;
  params["username"] = username;
  params["PIN"] = pin;
  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<UserInfo> ui(new UserInfo());
  ui->func = boost::bind(&Authentication::GetUserInfoCallback, this, _1);
  ui->username = username;
  ui->pin = pin;

  sm_->LoadPacket(midPacket->PacketName(&params),
      boost::bind(&Authentication::GetMidCallback, this, _1, _2, ui));
  sm_->LoadPacket(smidPacket->PacketName(&params),
      boost::bind(&Authentication::GetSmidCallback, this, _1, _2, ui));

  while (user_info_result_ == kPendingResult)
    boost::this_thread::sleep(boost::posix_time::milliseconds(500));

  ss_->SetUsername(username);
  ss_->SetPin(pin);
  return user_info_result_;
}

void Authentication::GetMidCallback(const std::vector<std::string> &values,
                                    const ReturnCode &rc,
                                    boost::shared_ptr<UserInfo> ui) {
  bool smid;
  {
    boost::mutex::scoped_lock loch(ui->m);
    ui->mid_calledback = true;
    smid = ui->smid_calledback;
  }

  if (rc != kSuccess) {
    ss_->SetMidRid(0);
    if (smid && ss_->SmidRid() == 0) {
#ifdef DEBUG
      printf("Authentication::GetMidCallback - No MID or SMID.\n");
#endif
      ui->func(kUserDoesntExist);
    }
    return;
  }

#ifdef DEBUG
  if (values.size() != 1)
    printf("Authentication::GetMidCallback - Values: %d\n", values.size());
#endif
  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams info = midPacket->GetData(values[0], ui->username, ui->pin);
  ss_->SetMidRid(boost::any_cast<boost::uint32_t>(info["data"]));
  GetMidTmid(ui);
}

void Authentication::GetSmidCallback(const std::vector<std::string> &values,
                                     const ReturnCode &rc,
                                     boost::shared_ptr<UserInfo> ui) {
  bool mid;
  {
    boost::mutex::scoped_lock loch(ui->m);
    ui->smid_calledback = true;
    mid = ui->mid_calledback;
  }

  if (rc != kSuccess) {
    ss_->SetSmidRid(0);
    if (mid && ss_->MidRid() == 0) {
#ifdef DEBUG
      printf("Authentication::GetSMidCallback - No MID or SMID.\n");
#endif
      ui->func(kUserDoesntExist);
    }
    return;
  }

#ifdef DEBUG
  if (values.size() != 1)
    printf("Authentication::GetSMidCallback - Values: %d\n", values.size());
#endif
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams info = smidPacket->GetData(values[0], ui->username, ui->pin);
  ss_->SetSmidRid(boost::any_cast<boost::uint32_t>(info["data"]));
  GetSmidTmid(ui);
}

void Authentication::GetMidTmid(boost::shared_ptr<UserInfo> ui) {
  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams params;
  params["username"] = ui->username;
  params["PIN"] = ui->pin;
  params["rid"] = ss_->MidRid();
  sm_->LoadPacket(tmidPacket->PacketName(&params),
      boost::bind(&Authentication::GetMidTmidCallback, this, _1, _2, ui));
}

void Authentication::GetSmidTmid(boost::shared_ptr<UserInfo> ui) {
  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams params;
  params["username"] = ui->username;
  params["PIN"] = ui->pin;
  params["rid"] = ss_->SmidRid();
  sm_->LoadPacket(tmidPacket->PacketName(&params),
      boost::bind(&Authentication::GetSmidTmidCallback, this, _1, _2, ui));
}

void Authentication::GetMidTmidCallback(const std::vector<std::string> &values,
                                        const ReturnCode &rc,
                                        boost::shared_ptr<UserInfo> ui) {
  bool smid;
  {
    boost::mutex::scoped_lock loch(ui->m);
    ui->tmid_mid_calledback = true;
    smid = ui->tmid_smid_calledback;
  }

  if (rc != kSuccess) {
    ss_->SetTmidContent("");
    if (smid && ss_->SmidTmidContent() == "") {
#ifdef DEBUG
      printf("Authentication::GetMidTmidCallback - No TMIDS found.\n");
#endif
      ui->func(kAuthenticationError);
    }
    return;
  }

#ifdef DEBUG
  if (values.size() != 1)
    printf("Authentication::GetMidTmidCallback - Values: %d\n", values.size());
#endif
  ss_->SetTmidContent(values[0]);
  ui->func(kUserExists);
}

void Authentication::GetSmidTmidCallback(const std::vector<std::string> &values,
                                         const ReturnCode &rc,
                                         boost::shared_ptr<UserInfo> ui) {
  bool mid;
  {
    boost::mutex::scoped_lock loch(ui->m);
    ui->tmid_smid_calledback = true;
    mid = ui->tmid_mid_calledback;
  }

  if (rc != kSuccess) {
    ss_->SetTmidContent("");
    if (mid && ss_->TmidContent() == "") {
#ifdef DEBUG
      printf("Authentication::GetSmidTmidCallback - No TMIDS found.\n");
#endif
      ui->func(kAuthenticationError);
    }
    return;
  }

#ifdef DEBUG
  if (values.size() != 1)
    printf("Authentication::GetSmidTmidCallback - Values: %d\n", values.size());
#endif
  ss_->SetSmidTmidContent(values[0]);
}

int Authentication::GetUserData(const std::string &password,
                                std::string *ser_da) {
  //  still have not recovered the tmid
  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams rec_data = tmidPacket->GetData(ss_->TmidContent(), password,
                          ss_->MidRid());
  *ser_da = boost::any_cast<std::string>(rec_data["data"]);

  DataMap dm;
  if (!dm.ParseFromString(*ser_da)) {
#ifdef DEBUG
    printf("Authentication::GetUserData - Ser DM doesn't parse (%s).\n",
           HexSubstr(ss_->TmidContent()).c_str());
    GenericPacket gp;
    if (gp.ParseFromString(*ser_da))
      printf("Authentication::GetUserData - GP en GP\n");
#endif
    return kPasswordFailure;
  }
  ss_->SetPassword(password);
  return kSuccess;
}

int Authentication::CreateUserSysPackets(const std::string &username,
                                         const std::string &pin) {
  system_packets_result_ = kPendingResult;
  PacketParams params;
  params["username"] = username;
  params["PIN"] = pin;
  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));

  boost::uint16_t count(0);
  bool calledback(false);
  VoidFuncOneInt func = boost::bind(
      &Authentication::CreateSystemPacketsCallback, this, _1);
  sm_->KeyUnique(midPacket->PacketName(&params), false, boost::bind(
      &Authentication::CreateUserSysPackets, this, _1, username, pin, func,
      &count, &calledback));
  sm_->KeyUnique(smidPacket->PacketName(&params), false, boost::bind(
      &Authentication::CreateUserSysPackets, this, _1, username, pin, func,
      &count, &calledback));

  while (system_packets_result_ == kPendingResult)
    boost::this_thread::sleep(boost::posix_time::milliseconds(500));

  return system_packets_result_;
}

void Authentication::CreateUserSysPackets(const ReturnCode rc,
                                          const std::string &username,
                                          const std::string &pin,
                                          VoidFuncOneInt vfoi,
                                          boost::uint16_t *count,
                                          bool *calledback) {
  if (*calledback)
    return;
  if (rc == kKeyUnique) {
    if (*count == 0) {
      ++*count;
      return;
    }
  } else {
    *calledback = true;
    vfoi(kUserExists);
    return;
  }
  boost::array<PacketType, 3> anonymous = { {ANMAID, ANMID, ANTMID} };
  boost::shared_ptr<SystemPacketCreation> data(new SystemPacketCreation());
  data->username = username;
  data->pin = pin;
  data->vfoi = vfoi;
  for (size_t n = 0; n < anonymous.size(); ++n) {
    CreateSignaturePacket(data, anonymous[n]);
  }
}

void Authentication::CreateSignaturePacket(
    boost::shared_ptr<SystemPacketCreation> spc,
    const PacketType &type_da) {
  PacketParams params;
  boost::shared_ptr<SignaturePacket> sigPacket(
      boost::static_pointer_cast<SignaturePacket>(PacketFactory::Factory(
      type_da, crypto_key_pairs_.GetKeyPair())));
  sigPacket->Create(&params);
  boost::shared_ptr<FindSystemPacket> fsp(new FindSystemPacket());
  fsp->spc = spc;
  fsp->pp = params;
  fsp->pt = type_da;

  VoidFuncOneInt func = boost::bind(
      &Authentication::CreateSignaturePacketKeyUnique, this, _1, fsp);
  sm_->KeyUnique(boost::any_cast<std::string>(params["name"]), false, func);
}

void Authentication::CreateSignaturePacketKeyUnique(
    const ReturnCode &rc,
    boost::shared_ptr<FindSystemPacket> fsp) {
  if (rc == kKeyUnique) {
    int n = ss_->AddKey(fsp->pt,
                        boost::any_cast<std::string>(fsp->pp["name"]),
                        boost::any_cast<std::string>(fsp->pp["privateKey"]),
                        boost::any_cast<std::string>(fsp->pp["publicKey"]),
                        "");
    if (n != 0) {
      // return to CreateSignaturePacket
    }

    VoidFuncOneInt func = boost::bind(
        &Authentication::CreateSignaturePacketStore, this, _1, fsp);
    sm_->StorePacket(boost::any_cast<std::string>(fsp->pp["name"]),
                     boost::any_cast<std::string>(fsp->pp["publicKey"]),
                     fsp->pt, PRIVATE, "", kDoNothingReturnFailure, func);
  } else {
    // return to CreateSignaturePacket
  }
}

void Authentication::CreateSignaturePacketStore(
    const ReturnCode &rc,
    boost::shared_ptr<FindSystemPacket> fsp) {
  if (rc == kSuccess) {
    ++fsp->spc->packet_count;
    switch (fsp->pt) {
      case ANMAID:
        fsp->pt = MAID;
        CreateMaidPmidPacket(fsp);
        break;
      case ANMID:
        CreateMidPacket(fsp);
        break;
      case MID:
        CreateSignaturePacket(fsp->spc, ANSMID);
        break;
      case ANSMID:
        CreateSmidPacket(fsp);
        break;
      case MAID:
        fsp->pt = PMID;
        CreateMaidPmidPacket(fsp);
        break;
      default:
        if (fsp->spc->packet_count == kNoOfSystemPackets)
          fsp->spc->vfoi(kSuccess);
    }
  } else {
    ss_->RemoveKey(fsp->pt);
  }
}

void Authentication::CreateMidPacket(boost::shared_ptr<FindSystemPacket> fsp) {
  fsp->pt = MID;
  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams user_params;
  user_params["username"] = fsp->spc->username;
  user_params["PIN"] = fsp->spc->pin;
  user_params["privateKey"] = ss_->PrivateKey(ANMID);
  std::string public_key;

  PacketParams mid_result = midPacket->Create(&user_params);
  ss_->SetMidRid(boost::any_cast<boost::uint32_t>(mid_result["rid"]));

  sm_->StorePacket(boost::any_cast<std::string>(mid_result["name"]),
                   boost::any_cast<std::string>(mid_result["encRid"]),
                   MID, PRIVATE, "", kDoNothingReturnFailure,
                   boost::bind(&Authentication::CreateSignaturePacketStore,
                               this, _1, fsp));
}

void Authentication::CreateSmidPacket(boost::shared_ptr<FindSystemPacket> fsp) {
  fsp->pt = SMID;
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams user_params;
  user_params["username"] = fsp->spc->username;
  user_params["PIN"] = fsp->spc->pin;
  user_params["privateKey"] = ss_->PrivateKey(ANSMID);
  user_params["rid"] = ss_->MidRid();
  std::string public_key;

  PacketParams smid_result = smidPacket->Create(&user_params);
  ss_->SetSmidRid(ss_->MidRid());
  sm_->StorePacket(boost::any_cast<std::string>(smid_result["name"]),
                   boost::any_cast<std::string>(smid_result["encRid"]),
                   SMID, PRIVATE, "", kDoNothingReturnFailure,
                   boost::bind(&Authentication::CreateSignaturePacketStore,
                               this, _1, fsp));
}

void Authentication::CreateMaidPmidPacket(
    boost::shared_ptr<FindSystemPacket> fsp) {
  boost::shared_ptr<PmidPacket> packet(
    boost::static_pointer_cast<PmidPacket>(PacketFactory::Factory(PMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams user_params;
  if (fsp->pt == PMID) {
    user_params["privateKey"] = ss_->PrivateKey(MAID);
  } else {
    user_params["privateKey"] = ss_->PrivateKey(ANMAID);
  }

  PacketParams result = packet->Create(&user_params);
  int n = ss_->AddKey(fsp->pt,
                      boost::any_cast<std::string>(result["name"]),
                      boost::any_cast<std::string>(result["privateKey"]),
                      boost::any_cast<std::string>(result["publicKey"]),
                      boost::any_cast<std::string>(result["signature"]));
  if (n != 0) {
    // return to CreateSignaturePacket
  }
  sm_->StorePacket(boost::any_cast<std::string>(result["name"]),
                   boost::any_cast<std::string>(result["publicKey"]),
                   fsp->pt, PRIVATE, "", kDoNothingReturnFailure,
                   boost::bind(&Authentication::CreateSignaturePacketStore,
                               this, _1, fsp));
}

int Authentication::CreateTmidPacket(const std::string &username,
                                     const std::string &pin,
                                     const std::string &password,
                                     const std::string &ser_dm) {
  PacketParams user_params;
  user_params["username"] = username;
  user_params["PIN"] = pin;
  user_params["privateKey"] = ss_->PrivateKey(ANTMID);
  user_params["password"] = password;
  user_params["rid"] = ss_->MidRid();

  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));

  // STORING SERLIALISED DATA MAP OF DATA ATLAS
  user_params["data"] = ser_dm;
  PacketParams tmid_result = tmidPacket->Create(&user_params);
  std::string enc_tmid(boost::any_cast<std::string>(tmid_result["data"]));
  std::string name_tmid(boost::any_cast<std::string>(tmid_result["name"]));
  std::string ser_tmid(boost::any_cast<std::string>(tmid_result["ser_packet"]));
  if (StorePacket(name_tmid, enc_tmid, TMID, kDoNothingReturnFailure, "")
      != kSuccess) {
    ss_->SetMidRid(0);
    ss_->SetSmidRid(0);
    return kAuthenticationError;
  }

  ss_->SetUsername(username);
  ss_->SetPin(pin);
  ss_->SetPassword(password);
  ss_->SetTmidContent(ser_tmid);
  ss_->SetSmidTmidContent(ser_tmid);

  return kSuccess;
}

int Authentication::SaveSession(const std::string &ser_da) {
  PacketParams params;
  PacketParams result;
  params["username"] = ss_->Username();
  params["PIN"] = ss_->Pin();

  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));
  if (ss_->MidRid() != ss_->SmidRid()) {
    params["rid"] = ss_->MidRid();
    params["privateKey"] = ss_->PrivateKey(ANSMID);
    result = smidPacket->Create(&params);
    if (StorePacket(boost::any_cast<std::string>(result["name"]),
        boost::any_cast<std::string>(result["encRid"]), SMID, kOverwrite, "")
        != kSuccess) {
      return kAuthenticationError;
    }

    params["rid"] = ss_->SmidRid();
    std::string tmidname(tmidPacket->PacketName(&params));
    GenericPacket gp;
    gp.ParseFromString(ss_->SmidTmidContent());
    if (DeletePacket(tmidname, gp.data(), TMID) != kSuccess) {
      return kAuthenticationError;
    }
    ss_->SetSmidRid(ss_->MidRid());
    ss_->SetSmidTmidContent(ss_->TmidContent());
  }

  params["privateKey"] = ss_->PrivateKey(ANMID);
  PacketParams mid_result = midPacket->Create(&params);
  while (ss_->MidRid() == boost::any_cast<boost::uint32_t>(mid_result["rid"]))
    mid_result = midPacket->Create(&params);

  params["privateKey"] = ss_->PrivateKey(ANTMID);
  params["rid"] = boost::any_cast<boost::uint32_t>(mid_result["rid"]);
  params["password"] = ss_->Password();
  params["data"] = ser_da;
  PacketParams tmidresult = tmidPacket->Create(&params);
  if (StorePacket(boost::any_cast<std::string>(tmidresult["name"]),
      boost::any_cast<std::string>(tmidresult["data"]), TMID,
      kDoNothingReturnFailure, "") != kSuccess) {
    return kAuthenticationError;
  }

  ss_->SetTmidContent(boost::any_cast<std::string>(tmidresult["ser_packet"]));

  if (StorePacket(boost::any_cast<std::string>(mid_result["name"]),
      boost::any_cast<std::string>(mid_result["encRid"]), MID, kOverwrite, "")
      != kSuccess) {
    return kAuthenticationError;
  }

  ss_->SetMidRid(boost::any_cast<boost::uint32_t>(mid_result["rid"]));
  return kSuccess;
}

int Authentication::RemoveMe(std::list<KeyAtlasRow> sig_keys) {
  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));

  PacketParams params;
  params["username"] = ss_->Username();
  params["PIN"] = ss_->Pin();

  params["rid"] = ss_->MidRid();
  std::string mpid_name, pmid_name;

  while (!sig_keys.empty()) {
    KeyAtlasRow kt = sig_keys.front();
    sig_keys.pop_front();
    switch (kt.type_) {
      case ANMID:
          DeletePacket(midPacket->PacketName(&params), "",
                       static_cast<PacketType>(kt.type_));
          break;
      case ANSMID:
          DeletePacket(smidPacket->PacketName(&params), "",
                       static_cast<PacketType>(kt.type_));
          break;
      case ANTMID:
          DeletePacket(tmidPacket->PacketName(&params), "",
                       static_cast<PacketType>(kt.type_));
          params["rid"] = ss_->SmidRid();
          if (ss_->SmidRid() != ss_->MidRid())
            DeletePacket(tmidPacket->PacketName(&params), "",
                         static_cast<PacketType>(kt.type_));
          break;
      case ANMPID:
          DeletePacket(mpid_name, "", static_cast<PacketType>(kt.type_));
          break;
      case MAID:
          if (!pmid_name.empty())
            DeletePacket(pmid_name, "", static_cast<PacketType>(kt.type_));
          break;
      case MPID: mpid_name = kt.id_; break;
      case PMID: pmid_name = kt.id_; break;
    }
    DeletePacket(kt.id_, "", static_cast<PacketType>(kt.type_));
  }
  return kSuccess;
}

int Authentication::CreatePublicName(const std::string &public_username) {
  PacketParams params;
  PacketParams local_result;
  params["publicname"] = public_username;
  boost::shared_ptr<MpidPacket> mpidPacket(
      boost::static_pointer_cast<MpidPacket>(PacketFactory::Factory(MPID,
      crypto_key_pairs_.GetKeyPair())));
  std::string mpidname = mpidPacket->PacketName(&params);

  if (!sm_->KeyUnique(mpidname, false)) {
#ifdef DEBUG
    printf("Authentication::CreatePublicName - Exists.\n");
#endif
    return kPublicUsernameExists;
  }

  boost::shared_ptr<SignaturePacket> sigPacket(
      boost::static_pointer_cast<SignaturePacket>(PacketFactory::Factory(ANMPID,
      crypto_key_pairs_.GetKeyPair())));
  sigPacket->Create(&params);
  while (!sm_->KeyUnique(boost::any_cast<std::string>(params["name"]),
         false))
    sigPacket->Create(&params);

  ss_->AddKey(ANMPID, boost::any_cast<std::string>(params["name"]),
              boost::any_cast<std::string>(params["privateKey"]),
              boost::any_cast<std::string>(params["publicKey"]),
              "");
  if (StorePacket(boost::any_cast<std::string>(params["name"]),
      boost::any_cast<std::string>(params["ser_packet"]), ANMPID,
      kDoNothingReturnFailure, "") != kSuccess) {
#ifdef DEBUG
    printf("Authentication::CreatePublicName - Buggered in ANMPID\n");
#endif
    ss_->RemoveKey(ANMPID);
    return kAuthenticationError;
  }

  PacketParams mpid_result = mpidPacket->Create(&params);
  std::string data = boost::any_cast<std::string>(mpid_result["publicKey"]);
  std::string pubkey_signature = crypto_.AsymSign(data, "",
                                 ss_->PrivateKey(ANMPID),
                                 crypto::STRING_STRING);

  if (StorePacket(boost::any_cast<std::string>(mpid_result["name"]), data,
      MPID, kDoNothingReturnFailure, "") != kSuccess) {
#ifdef DEBUG
    printf("Authentication::CreatePublicName - Buggered in MPID\n");
#endif
    ss_->RemoveKey(ANMPID);
    return kAuthenticationError;
  }

  ss_->AddKey(MPID,
              public_username,
              boost::any_cast<std::string>(mpid_result["privateKey"]),
              boost::any_cast<std::string>(mpid_result["publicKey"]),
              pubkey_signature);

  return kSuccess;
}

int Authentication::ChangeUsername(const std::string &ser_da,
                                   const std::string &new_username) {
//  if (!CheckUsername(new_username) || new_username == ss_->Username())
//    return kUserExists;  // INVALID_USERNAME;

  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams user_params;
  user_params["username"] = ss_->Username();
  user_params["PIN"] = ss_->Pin();

  // Backing up current session details
  boost::uint32_t old_mid(ss_->MidRid());
  boost::uint32_t old_smid(ss_->SmidRid());
  std::string old_mid_name(midPacket->PacketName(&user_params));
  std::string old_smid_name(smidPacket->PacketName(&user_params));
  std::string old_mid_tmid(ss_->TmidContent());
  std::string old_smid_tmid(ss_->SmidTmidContent());
  user_params["rid"] = old_mid;
  std::string old_mid_tmid_name(tmidPacket->PacketName(&user_params));
  user_params["rid"] = old_smid;
  std::string old_smid_tmid_name(tmidPacket->PacketName(&user_params));
  std::string old_username(ss_->Username());

  // Verifying uniqueness of new MID
  user_params["username"] = new_username;
  user_params["PIN"] = ss_->Pin();
  std::string mid_name = midPacket->PacketName(&user_params);
  if (!sm_->KeyUnique(mid_name, false))
    return kUserExists;

  // Verifying uniqueness of new SMID
  std::string smid_name = smidPacket->PacketName(&user_params);
  if (!sm_->KeyUnique(smid_name, false))
    return kUserExists;

  //  Creating and storing new MID packet with new username
  user_params["privateKey"] = ss_->PrivateKey(ANMID);
  PacketParams mid_result = midPacket->Create(&user_params);
  while (ss_->MidRid() == boost::any_cast<boost::uint32_t>(mid_result["rid"]))
    mid_result = midPacket->Create(&user_params);

  if (StorePacket(boost::any_cast<std::string>(mid_result["name"]),
      boost::any_cast<std::string>(mid_result["encRid"]), MID,
      kDoNothingReturnFailure, "") != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername: Can't store new MID\n");
#endif
    return kAuthenticationError;
  }

  //  Creating and storing new SMID packet with new username
  user_params["privateKey"] = ss_->PrivateKey(ANSMID);
  boost::uint32_t new_smid_rid(0);
  while (new_smid_rid == 0 || new_smid_rid == old_mid ||
         new_smid_rid == old_smid) {
    new_smid_rid = base::random_32bit_uinteger();
  }
  user_params["rid"] = new_smid_rid;
  PacketParams smid_result = smidPacket->Create(&user_params);
  if (StorePacket(boost::any_cast<std::string>(smid_result["name"]),
      boost::any_cast<std::string>(smid_result["encRid"]), SMID,
      kDoNothingReturnFailure, "") != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername: Can't store new SMID\n");
#endif
    return kAuthenticationError;
  }

  //  Creating new MID TMID
  user_params["privateKey"] = ss_->PrivateKey(ANTMID);
  user_params["password"] = ss_->Password();
  user_params["rid"] = boost::any_cast<boost::uint32_t>(mid_result["rid"]);
  user_params["data"] = ser_da;
  PacketParams tmid_result = tmidPacket->Create(&user_params);
  std::string new_mid_tmid(boost::any_cast<std::string>(tmid_result["data"]));
  if (StorePacket(boost::any_cast<std::string>(tmid_result["name"]),
      boost::any_cast<std::string>(tmid_result["data"]), TMID,
      kDoNothingReturnFailure, "") != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername: Can't store new TMID\n");
#endif
    return kAuthenticationError;
  }

  //  Creating new SMID TMID
  PacketParams old_user_params;
  old_user_params["username"] = ss_->Username();
  old_user_params["PIN"] = ss_->Pin();
  old_user_params["rid"] = ss_->MidRid();
  PacketParams rec_tmid = tmidPacket->GetData(old_mid_tmid, ss_->Password(),
                          ss_->MidRid());
  std::string tmid_data = boost::any_cast<std::string>(rec_tmid["data"]);
  if (tmid_data.empty())
    return kAuthenticationError;
  old_user_params["data"] = tmid_data;
  old_user_params["privateKey"] = ss_->PrivateKey(ANTMID);
  old_user_params["password"] = ss_->Password();
  old_user_params["username"] = new_username;
  old_user_params["rid"] = new_smid_rid;
  tmid_result = tmidPacket->Create(&old_user_params);
  std::string new_smid_tmid(boost::any_cast<std::string>(tmid_result["data"]));
  if (StorePacket(boost::any_cast<std::string>(tmid_result["name"]),
      new_smid_tmid, TMID, kDoNothingReturnFailure, "") != kSuccess) {
    return kAuthenticationError;
  }

  // Deleting old MID
  int result = DeletePacket(old_mid_name, EncryptedDataMidSmid(old_mid), MID);
  if (result != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername - Failed to delete MID.\n");
#endif
    return kAuthenticationError;
  }

  // Deleting old SMID
  result = DeletePacket(old_smid_name, EncryptedDataMidSmid(old_smid), SMID);
  if (result != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername - Failed to delete SMID.\n");
#endif
    return kAuthenticationError;
  }

  // Deleting old MID TMID
  GenericPacket gp;
  std::string tmidcontent;
  if (gp.ParseFromString(ss_->TmidContent()))
    tmidcontent = gp.data();
  result = DeletePacket(old_mid_tmid_name, tmidcontent, TMID);
  if (result != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername - Failed to delete midTMID {%s}.\n",
           ss_->TmidContent().c_str());
#endif
    return kAuthenticationError;
  }

  // Deleting old SMID TMID
  if (ss_->MidRid() != ss_->SmidRid()) {
    gp.Clear();
    if (gp.ParseFromString(ss_->SmidTmidContent()))
      tmidcontent = gp.data();
    result = DeletePacket(old_smid_tmid_name, tmidcontent, TMID);
    if (result != kSuccess) {
#ifdef DEBUG
      printf("Authentication::ChangeUsername - Failed to delete smidTMID.\n");
#endif
      return kAuthenticationError;
    }
  }

  ss_->SetUsername(new_username);
  ss_->SetSmidRid(new_smid_rid);
  ss_->SetMidRid(boost::any_cast<boost::uint32_t>(mid_result["rid"]));
  ss_->SetTmidContent(new_mid_tmid);
  ss_->SetSmidTmidContent(new_smid_tmid);

  return kSuccess;
}

int Authentication::ChangePin(const std::string &ser_da,
                              const std::string &new_pin) {
//  if (!CheckUsername(new_username) || new_username == ss_->Username())
//    return kUserExists;  // INVALID_USERNAME;

  boost::shared_ptr<MidPacket> midPacket(
      boost::static_pointer_cast<MidPacket>(PacketFactory::Factory(MID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<SmidPacket> smidPacket(
      boost::static_pointer_cast<SmidPacket>(PacketFactory::Factory(SMID,
      crypto_key_pairs_.GetKeyPair())));
  boost::shared_ptr<TmidPacket> tmidPacket(
      boost::static_pointer_cast<TmidPacket>(PacketFactory::Factory(TMID,
      crypto_key_pairs_.GetKeyPair())));
  PacketParams user_params;
  user_params["username"] = ss_->Username();
  user_params["PIN"] = ss_->Pin();

  // Backing up current session details
  boost::uint32_t old_mid(ss_->MidRid());
  boost::uint32_t old_smid(ss_->SmidRid());
  std::string old_mid_name(midPacket->PacketName(&user_params));
  std::string old_smid_name(smidPacket->PacketName(&user_params));
  std::string old_mid_tmid(ss_->TmidContent());
  std::string old_smid_tmid(ss_->SmidTmidContent());
  user_params["rid"] = old_mid;
  std::string old_mid_tmid_name(tmidPacket->PacketName(&user_params));
  user_params["rid"] = old_smid;
  std::string old_smid_tmid_name(tmidPacket->PacketName(&user_params));
  std::string old_pin(ss_->Pin());

  // Verifying uniqueness of new MID
  user_params["username"] = ss_->Username();
  user_params["PIN"] = new_pin;
  std::string mid_name = midPacket->PacketName(&user_params);
  if (!sm_->KeyUnique(mid_name, false))
    return kUserExists;

  // Verifying uniqueness of new SMID
  std::string smid_name = smidPacket->PacketName(&user_params);
  if (!sm_->KeyUnique(smid_name, false))
    return kUserExists;

  //  Creating and storing new MID packet with new username
  user_params["privateKey"] = ss_->PrivateKey(ANMID);
  PacketParams mid_result = midPacket->Create(&user_params);
  while (ss_->MidRid() == boost::any_cast<boost::uint32_t>(mid_result["rid"]))
    mid_result = midPacket->Create(&user_params);

  if (StorePacket(boost::any_cast<std::string>(mid_result["name"]),
      boost::any_cast<std::string>(mid_result["encRid"]), MID,
      kDoNothingReturnFailure, "") != kSuccess) {
    return kAuthenticationError;
  }

  //  Creating and storing new SMID packet with new username
  user_params["privateKey"] = ss_->PrivateKey(ANSMID);
  boost::uint32_t new_smid_rid(0);
  while (new_smid_rid == 0 || new_smid_rid == old_mid ||
         new_smid_rid == old_smid) {
    new_smid_rid = base::random_32bit_uinteger();
  }
  user_params["rid"] = new_smid_rid;
  PacketParams smid_result = smidPacket->Create(&user_params);
  if (StorePacket(boost::any_cast<std::string>(smid_result["name"]),
      boost::any_cast<std::string>(smid_result["encRid"]), SMID,
      kDoNothingReturnFailure, "") != kSuccess) {
    return kAuthenticationError;
  }

  //  Creating new MID TMID
  user_params["privateKey"] = ss_->PrivateKey(ANTMID);
  user_params["password"] = ss_->Password();
  user_params["rid"] = boost::any_cast<boost::uint32_t>(mid_result["rid"]);
  user_params["data"] = ser_da;
  PacketParams tmid_result = tmidPacket->Create(&user_params);
  std::string new_mid_tmid(boost::any_cast<std::string>(tmid_result["data"]));
  if (StorePacket(boost::any_cast<std::string>(tmid_result["name"]),
      boost::any_cast<std::string>(tmid_result["data"]), TMID,
      kDoNothingReturnFailure, "") != kSuccess) {
    return kAuthenticationError;
  }

  //  Creating new SMID TMID
  PacketParams old_user_params;
  old_user_params["username"] = ss_->Username();
  old_user_params["PIN"] = ss_->Pin();
  old_user_params["rid"] = ss_->MidRid();
  PacketParams rec_tmid = tmidPacket->GetData(old_mid_tmid, ss_->Password(),
                          ss_->MidRid());
  std::string tmid_data = boost::any_cast<std::string>(rec_tmid["data"]);
  if (tmid_data.empty())
    return kAuthenticationError;
  old_user_params["data"] = tmid_data;
  old_user_params["privateKey"] = ss_->PrivateKey(ANTMID);
  old_user_params["password"] = ss_->Password();
  old_user_params["PIN"] = new_pin;
  old_user_params["rid"] = new_smid_rid;
  tmid_result = tmidPacket->Create(&old_user_params);
  std::string new_smid_tmid(boost::any_cast<std::string>(tmid_result["data"]));
  if (StorePacket(boost::any_cast<std::string>(tmid_result["name"]),
      new_smid_tmid, TMID, kDoNothingReturnFailure, "") != kSuccess) {
    return kAuthenticationError;
  }

  // Deleting old MID
  int result = DeletePacket(old_mid_name, EncryptedDataMidSmid(old_mid), MID);
  if (result != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername - Failed to delete MID.\n");
#endif
    return kAuthenticationError;
  }

  // Deleting old SMID
  result = DeletePacket(old_smid_name, EncryptedDataMidSmid(old_smid), SMID);
  if (result != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername - Failed to delete SMID.\n");
#endif
    return kAuthenticationError;
  }

  // Deleting old MID TMID
  GenericPacket gp;
  std::string tmidcontent;
  if (gp.ParseFromString(ss_->TmidContent()))
    tmidcontent = gp.data();
  result = DeletePacket(old_mid_tmid_name, tmidcontent, TMID);
  if (result != kSuccess) {
#ifdef DEBUG
    printf("Authentication::ChangeUsername - Failed to delete midTMID {%s}.\n",
           ss_->TmidContent().c_str());
#endif
    return kAuthenticationError;
  }

  // Deleting old SMID TMID
  if (ss_->MidRid() != ss_->SmidRid()) {
    gp.Clear();
    if (gp.ParseFromString(ss_->SmidTmidContent()))
      tmidcontent = gp.data();
    result = DeletePacket(old_smid_tmid_name, tmidcontent, TMID);
    if (result != kSuccess) {
#ifdef DEBUG
      printf("Authentication::ChangeUsername - Failed to delete smidTMID.\n");
#endif
      return kAuthenticationError;
    }
  }

  ss_->SetPin(new_pin);
  ss_->SetSmidRid(new_smid_rid);
  ss_->SetMidRid(boost::any_cast<boost::uint32_t>(mid_result["rid"]));
  ss_->SetTmidContent(new_mid_tmid);
  ss_->SetSmidTmidContent(new_smid_tmid);

  return kSuccess;
}

int Authentication::ChangePassword(const std::string &ser_da,
                                   const std::string &new_password) {
//  if (!CheckPassword(new_password) || new_password == ss_->Password())
//    return INVALID_PASSWORD;
  std::string old_password = ss_->Password();
  ss_->SetPassword(new_password);
  if (SaveSession(ser_da) == kSuccess) {
    return kSuccess;
  } else {
    ss_->SetPassword(old_password);
    return kAuthenticationError;
  }
}

std::string Authentication::CreateSignaturePackets(const PacketType &type_da,
                                                   std::string *public_key) {
  PacketParams params;
  boost::shared_ptr<SignaturePacket> sigPacket(
      boost::static_pointer_cast<SignaturePacket>(PacketFactory::Factory(
      type_da, crypto_key_pairs_.GetKeyPair())));
  sigPacket->Create(&params);

  while (!sm_->KeyUnique(boost::any_cast<std::string>(params["name"]), false))
    sigPacket->Create(&params);

  ss_->AddKey(type_da,
              boost::any_cast<std::string>(params["name"]),
              boost::any_cast<std::string>(params["privateKey"]),
              boost::any_cast<std::string>(params["publicKey"]),
              "");

  if (StorePacket(boost::any_cast<std::string>(params["name"]),
      boost::any_cast<std::string>(params["ser_packet"]), type_da,
      kDoNothingReturnFailure, "") != kSuccess) {
    ss_->RemoveKey(type_da);
    return "";
  }

  *public_key = boost::any_cast<std::string>(params["publicKey"]);
  return boost::any_cast<std::string>(params["privateKey"]);
}

bool Authentication::CheckUsername(const std::string &username) {
  std::string username_ = utils_trim(boost::lexical_cast<char*>(username));
  return (username_.length() >= 4);
}

bool Authentication::CheckPin(const std::string &pin) {
  std::string pin_ = utils_trim(boost::lexical_cast<char*>(pin));
  if (pin_ == "0000")
    return false;
  boost::regex re("\\d{4}");
  return boost::regex_match(pin_, re);
}

bool Authentication::CheckPassword(const std::string &password) {
  std::string password_ = utils_trim(boost::lexical_cast<char*>(password));
  return (password_.length() >= 4);
}

int Authentication::PublicUsernamePublicKey(const std::string &public_username,
                                            std::string *public_key) {
  PacketParams params;
  params["publicname"] = public_username;
  boost::shared_ptr<MpidPacket> mpidPacket(
      boost::static_pointer_cast<MpidPacket>(PacketFactory::Factory(MPID,
      crypto_key_pairs_.GetKeyPair())));

  std::vector<std::string> packet_content;
  int result = sm_->LoadPacket(mpidPacket->PacketName(&params),
                                         &packet_content);
  if (result != kSuccess || packet_content.empty())
    return kUserDoesntExist;
  std::string ser_generic_packet = packet_content[0];
  GenericPacket gp;
  if (!gp.ParseFromString(ser_generic_packet)) {
    return kAuthenticationError;  // Packet corrupt
  }

  *public_key = gp.data();

  return kSuccess;
}

void Authentication::CreateMSIDPacket(base::callback_func_type cb) {
  PacketParams params;
  boost::shared_ptr<SignaturePacket> sigPacket(
      boost::static_pointer_cast<SignaturePacket>(PacketFactory::Factory(MSID,
      crypto_key_pairs_.GetKeyPair())));
  sigPacket->Create(&params);

  int count = 0;
  while (!sm_->KeyUnique(boost::any_cast<std::string>(params["name"]),
         false) && count < 10)
    ++count;

  if (count > 9) {
    CreateMSIDResult local_result;
    local_result.set_result(kNack);
    std::string ser_local_result;
    local_result.SerializeToString(&ser_local_result);
    cb(ser_local_result);
    return;
  }

  std::vector<boost::uint32_t> share_stats(2, 0);
  std::vector<std::string> atts;
  atts.push_back(boost::any_cast<std::string>(params["name"]));
  atts.push_back(boost::any_cast<std::string>(params["name"]));
  atts.push_back(boost::any_cast<std::string>(params["publicKey"]));
  atts.push_back(boost::any_cast<std::string>(params["privateKey"]));
  int n = ss_->AddPrivateShare(atts, share_stats, NULL);

  n = StorePacket(boost::any_cast<std::string>(params["name"]),
      boost::any_cast<std::string>(params["publicKey"]), MSID,
      kDoNothingReturnFailure, boost::any_cast<std::string>(params["name"]));
  ss_->DeletePrivateShare(atts[0], 0);

  StoreChunkResponse result_msg;
  CreateMSIDResult local_result;
  std::string str_local_result;
  if (n != 0) {
    local_result.set_result(kNack);
  } else {
    local_result.set_result(kAck);
    local_result.set_private_key(boost::any_cast<std::string>(
        params["privateKey"]));
    local_result.set_public_key(boost::any_cast<std::string>(
        params["publicKey"]));
    local_result.set_name(boost::any_cast<std::string>(params["name"]));
  }
  local_result.SerializeToString(&str_local_result);
  cb(str_local_result);
}

int Authentication::StorePacket(const std::string &packet_name,
                                const std::string &value,
                                const PacketType &type,
                                const IfPacketExists &if_exists,
                                const std::string &msid) {
// TODO(Fraser#5#): 2010-01-28 - Use callbacks properly to allow several stores
//                               to happen concurrently.
  boost::mutex mutex;
  boost::condition_variable cond_var;
  int result(kGeneralError);
  VoidFuncOneInt func = boost::bind(&Authentication::PacketOpCallback, this, _1,
                                    &mutex, &cond_var, &result);
  sm_->StorePacket(packet_name, value, type, PRIVATE_SHARE, msid, if_exists,
      func);
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == kGeneralError)
      cond_var.wait(lock);
  }
  return result;
}

int Authentication::DeletePacket(const std::string &packet_name,
                                 const std::string &value,
                                 const PacketType &type) {
// TODO(Fraser#5#): 2010-01-28 - Use callbacks properly to allow several deletes
//                               to happen concurrently.
  boost::mutex mutex;
  boost::condition_variable cond_var;
  int result(kGeneralError);
  VoidFuncOneInt func = boost::bind(&Authentication::PacketOpCallback, this, _1,
                                    &mutex, &cond_var, &result);
  std::vector<std::string> values;
  if ("" != value)
    values.push_back(value);
  sm_->DeletePacket(packet_name, values, type, PRIVATE, "", func);
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == kGeneralError)
      cond_var.wait(lock);
  }
  return result;
}

void Authentication::PacketOpCallback(const int &store_manager_result,
                                      boost::mutex *mutex,
                                      boost::condition_variable *cond_var,
                                      int *op_result) {
  boost::mutex::scoped_lock lock(*mutex);
  *op_result = store_manager_result;
  cond_var->notify_one();
}

std::string Authentication::EncryptedDataMidSmid(boost::uint32_t rid) {
  std::string password = crypto_.SecurePassword(ss_->Username(),
                         boost::lexical_cast<boost::uint16_t>(ss_->Pin()));
  return crypto_.SymmEncrypt(boost::lexical_cast<std::string>(rid), "",
                             crypto::STRING_STRING, password);
}

void Authentication::CreateSystemPacketsCallback(const ReturnCode &rc) {
  system_packets_result_ = rc;
}

void Authentication::GetUserInfoCallback(const ReturnCode &rc) {
  user_info_result_ = rc;
}

}  // namespace maidsafe
