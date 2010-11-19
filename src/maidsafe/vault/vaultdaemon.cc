/*
* ============================================================================
*
* Copyright [2009] maidsafe.net limited
*
* Description:  Daemon for running a PD vault.
* Version:      1.0
* Created:      2009-02-21-23.55.54
* Revision:     none
* Author:       Team
* Company:      maidsafe.net limited
*
* The following source code is property of maidsafe.net limited and is not
* meant for external use.  The use of this code is governed by the license
* file LICENSE.TXT found in the root of this directory and also on
* www.maidsafe.net.
*
* You are not free to copy, amend or otherwise use this source code without
* the explicit written permission of the board of directors of maidsafe.net.
*
* ============================================================================
*/

#include "maidsafe/vault/vaultdaemon.h"

#include <google/protobuf/descriptor.h>
#ifdef PD_WIN32
#include <shlwapi.h>
#endif
#include <fstream>  // NOLINT (Fraser)
#include "maidsafe/common/commonutils.h"
#include "maidsafe/common/filesystem.h"
#include "maidsafe/vault/pdvault.h"
#include "maidsafe/vault/vaultservice.h"
#include "maidsafe/common/maidsafe_messages.pb.h"

namespace fs = boost::filesystem;

//  #if defined(PD_APPLE)
//    int WriteToLog(std::string str) { return 0; }
//  #endif

namespace maidsafe {

namespace vault {

VaultDaemon::VaultDaemon(const int &port, const std::string &vault_dir,
                         const boost::uint8_t k)
    : pdvault_(),
      is_owned_(false),
      config_file_(),
      kad_config_file_(),
      app_path_(file_system::ApplicationDataDir()),
      not_owned_path_(),
      vault_path_(vault_dir),
      pmid_public_(),
      pmid_private_(),
      signed_pmid_public_(),
      port_(port),
      vault_available_space_(0),
      used_space_(0),
      local_udt_transport_(),
      global_udt_transport_(),
      transport_handler_(),
      channel_manager_(&transport_handler_),
      registration_channel_(),
      registration_service_(),
      config_mutex_(),
      K_(k),
      test_config_postfix_() {
  boost::int16_t trans_id;
  transport_handler_.Register(&local_udt_transport_, &trans_id);
  transport_handler_.Register(&global_udt_transport_, &trans_id);
}

VaultDaemon::~VaultDaemon() {
  std::string stop("VaultDaemon stopping ");
  StopRegistrationService();
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  stop += boost::posix_time::to_simple_string(now);
  WriteToLog(stop);
  if (pdvault_) {
    pdvault_->Stop();
    pdvault_->CleanUp();
  }
}

void VaultDaemon::StopRegistrationService() {
//  if (registration_service_) {
//    local_udt_transport_.Stop();
//    channel_manager_.ClearChannels();
//  }
  WriteToLog("Stopped registration service\n");
}

void VaultDaemon::Status() {
  WriteToLog("OK");
}

void VaultDaemon::RegistrationNotification(const VaultConfig &vconfig) {
  boost::mutex::scoped_lock gaurd(config_mutex_);
  std::fstream output(config_file_.string().c_str(),
      std::ios::out | std::ios::trunc | std::ios::binary);
  vconfig.SerializeToOstream(&output);
  output.close();
}

bool VaultDaemon::TakeOwnership() {
  while (!is_owned_) {
    int return_code = ReadConfigInfo();
    if (return_code == kSuccess) {
      if (StopNotOwnedVault() != kSuccess) {
        WriteToLog("Unowned vault won't stop.\n");
        StopRegistrationService();
        return false;
      }
      pdvault_.reset();
      if (!StartOwnedVault()) {
        registration_service_->ReplySetLocalVaultOwnedRequest(true);
        StartNotOwnedVault();
      } else {
        registration_service_->set_status(maidsafe::OWNED);
        registration_service_->ReplySetLocalVaultOwnedRequest(false);
        is_owned_ = true;
      }
    } else if (return_code == kVaultDaemonWaitingPwnage) {
      boost::this_thread::sleep(boost::posix_time::seconds(1.0));
    } else {
      StopRegistrationService();
      return false;
    }
  }
  WriteToLog("Vault has been owned.\n");
  WriteToLog("Vault ID:         " + base::EncodeToHex(pdvault_->pmid()));
//  WriteToLog("Vault IP & port:  " + pdvault_->host_ip()+":"+
//      base::IntToString(pdvault_->host_port()));
  StopRegistrationService();
  return true;
}

int VaultDaemon::SetPaths() {
  if (vault_path_.empty()) {
    vault_path_ = app_path_;
  }
  try {
    if (!fs::exists(vault_path_))
      fs::create_directories(vault_path_);
  }
  catch(const std::exception &ex_) {
    WriteToLog("Can't create maidsafe vault dir.");
    WriteToLog(ex_.what());
    return kVaultDaemonException;
  }
  config_file_ = app_path_ / (".config" + test_config_postfix_);
  kad_config_file_ = app_path_ / (".kadconfig" + test_config_postfix_);
  return kSuccess;
}

void VaultDaemon::SyncVault() {
  kad::VoidFunctorOneString cb;
  pdvault_->SyncVault(cb);
}

void VaultDaemon::RepublishChunkRef() {
  kad::VoidFunctorOneString cb;
  pdvault_->RepublishChunkRef(cb);
}

void VaultDaemon::ValidityCheck() {
//  kad::VoidFunctorOneString cb;
//  pdvault_->ValidityCheck(cb);
}

bool VaultDaemon::StartVault() {
  std::string init("VaultDaemon starting ");
  boost::posix_time::ptime now =
      boost::posix_time::second_clock::local_time();
  init += boost::posix_time::to_simple_string(now);
  WriteToLog(init);
  if (kSuccess != SetPaths()) {
    WriteToLog("Failed to set path to config file - can't start vault.\n");
    return false;
  }
  bool started_registration_service = true;
  if (!channel_manager_.RegisterNotifiersToTransport()) {
    started_registration_service = false;
  } else {
    registration_channel_.reset(new rpcprotocol::Channel(&channel_manager_,
        &transport_handler_));
    registration_service_.reset(new RegistrationService(
        boost::bind(&VaultDaemon::RegistrationNotification, this, _1)));
    registration_channel_->SetService(registration_service_.get());
    channel_manager_.RegisterChannel(
        registration_service_->GetDescriptor()->name(),
        registration_channel_.get());
    if (0 != transport_handler_.StartLocal(kLocalPort,
             local_udt_transport_.transport_id())) {
      WriteToLog("Failed to start transport on port " +
                 base::IntToString(kLocalPort));
      channel_manager_.ClearChannels();
      started_registration_service = false;
    }
  }
  if (ReadConfigInfo() != kSuccess) {
    if (!started_registration_service) {
      WriteToLog("Failed to start registration service");
      return false;
    }
    if (!StartNotOwnedVault())
      return false;
    else
      return TakeOwnership();
  } else {
    if (!StartOwnedVault()) {
      return false;
    } else {
      if (registration_service_)
        registration_service_->set_status(OWNED);
      is_owned_ = true;
    }
  }
  StopRegistrationService();
  return true;
}

int VaultDaemon::ReadConfigInfo() {
  try {
    boost::mutex::scoped_lock gaurd(config_mutex_);
    if (!fs::exists(config_file_))
      return kVaultDaemonWaitingPwnage;
  }
  catch(const std::exception &e) {
    std::string err("Can't access location for config file at ");
    err += config_file_.string();
    WriteToLog(err);
    WriteToLog(e.what());
    return kVaultDaemonException;
  }
  std::ifstream input(config_file_.string().c_str(),
                      std::ios::in | std::ios::binary);
  VaultConfig vault_config;
  if (!vault_config.ParseFromIstream(&input)) {
    WriteToLog("Failed to parse configuration file.\n");
    return kVaultDaemonParseError;
  }

  pmid_public_ = vault_config.pmid_public();
  pmid_private_ = vault_config.pmid_private();
  signed_pmid_public_ = vault_config.signed_pmid_public();

  if (pmid_public_.empty() || pmid_private_.empty() ||
      signed_pmid_public_.empty()) {
#ifdef DEBUG
    if (pmid_public_.empty())
      WriteToLog("vault_config.pmid_public() empty.\n");
    if (pmid_private_.empty())
      WriteToLog("vault_config.pmid_private() empty.\n");
    if (signed_pmid_public_.empty())
      WriteToLog("vault_config.signed_pmid_public() empty.\n");
#endif
    return kVaultDaemonConfigError;
  }
  vault_path_ = vault_config.vault_dir();
  vault_available_space_ = vault_config.available_space();
  if (vault_config.has_used_space())
    used_space_ = vault_config.used_space();
  else
    used_space_ = 0;
  // If a port between 5000 & 65535 inclusive is passed into VaultDaemon,
  // use that, otherwise try the config file.  As a last resort, set port to
  // 0 and PDVault will use a random port.
  if (vault_config.has_port() &&
      vault_config.port() >= kMinPort &&
      vault_config.port() <= kMaxPort)
    port_ = vault_config.port();
  else
    port_ = 0;
  WriteToLog("Found config file at " + config_file_.string());
  WriteToLog("Vault dir set to " + vault_path_.string());
  return kSuccess;
}

bool VaultDaemon::StartNotOwnedVault() {
  crypto::RsaKeyPair keys;
  keys.GenerateKeys(kRsaKeySize);
  std::string signed_pubkey = RSASign(keys.public_key(), keys.private_key());
  std::string temp_pmid = base::EncodeToHex(SHA512String(
      keys.public_key() + signed_pubkey));
  not_owned_path_ = app_path_ / ("Vault_" + temp_pmid.substr(0, 16));
  try {
    if (fs::exists(not_owned_path_))
      fs::remove_all(not_owned_path_);
    fs::create_directories(not_owned_path_);
  }
  catch(const std::exception &e) {
    WriteToLog("Can't create temp maidsafe vault dir.");
    WriteToLog(e.what());
    return false;
  }
  boost::uint64_t space(1024 * 1024 * 1024);  // 1GB
  pdvault_.reset(new PDVault(keys.public_key(), keys.private_key(),
                             signed_pubkey, not_owned_path_, 0, false, false,
                             kad_config_file_, space, 0, K_));
  // For testing, create empty .kadconfig in not_owned_path_ to avoid
  // overwriting real .kadconfig on vault Leave().
  if (!test_config_postfix_.empty()) {
    std::fstream output((not_owned_path_.string() + "/.kadconfig").c_str(),
        std::ios::out | std::ios::trunc | std::ios::binary);
    output.close();
  }
  pdvault_->Start(false);
  if (pdvault_->vault_status() == kVaultStopped) {
    WriteToLog("Failed to start a not owned vault - "
               "trying to create a new network.");
    pdvault_->Start(true);
    if (pdvault_->vault_status() == kVaultStopped) {
      WriteToLog("Failed to start a not owned vault.");
      return false;
    } else {
      WriteToLog("Vault started (New network).  Waiting to be owned.\n");
    }
  } else {
    WriteToLog("Vault started.  Waiting to be owned.\n");
  }
  WriteToLog("Vault ID:         " + base::EncodeToHex(pdvault_->pmid()));
//  WriteToLog("Vault IP & port:  " + pdvault_->host_ip() + ":" +
//             base::IntToString(pdvault_->host_port()));
  WriteToLog("Config file will be written to " + config_file_.string());
  return true;
}

int VaultDaemon::StopNotOwnedVault() {
  int result = pdvault_->Stop();
  // TODO(Fraser#5#): 2010-02-26 - Should the old chunkstore be transferred?
  if (result == kSuccess) {
    try {
      // TODO(Fraser#5#): 2010-06-24 - Merge this kadconfig with one which could
      //                               passed through as part of Config?
      fs::create_directories(vault_path_);
      fs::copy_file((not_owned_path_ / ".kadconfig"),
                    (vault_path_ / ".kadconfig"));
      fs::remove_all(not_owned_path_);
    }
    catch(const std::exception &e) {
#ifdef DEBUG
      printf("In VaultDaemon::StopNotOwnedVault, %s\n", e.what());
#endif
      result = kVaultDaemonException;
    }
  }
  return result;
}

bool VaultDaemon::StartOwnedVault() {
  if (pdvault_)
    return false;
  // If kadconfig already exists in vault dir, use that.  If not use the one
  // in app_dir_.  If neither exists, start a new network.
  bool first_vault(true);
  try {
    if (fs::exists(vault_path_ / ".kadconfig")) {
      kad_config_file_ = vault_path_ / ".kadconfig";
      first_vault = false;
    } else if (fs::exists(kad_config_file_)) {
      first_vault = false;
    }
  }
  catch(const std::exception &e) {
    WriteToLog("Failed To Start Owned Vault:");
    WriteToLog(e.what());
    return false;
  }
  pdvault_.reset(new PDVault(pmid_public_, pmid_private_, signed_pmid_public_,
                             vault_path_, port_, false, false, kad_config_file_,
                             vault_available_space_, used_space_, K_));
  pdvault_->Start(first_vault);
  if (pdvault_->vault_status() == kVaultStopped) {
    WriteToLog("Failed To Start Owned Vault with info in config file");
    return false;
  }
  registration_service_->set_status(OWNED);
  return true;
}

}  // namespace vault

}  // namespace maidsafe
