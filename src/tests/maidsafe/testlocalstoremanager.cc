/*
* ============================================================================
*
* Copyright [2009] maidsafe.net limited
*
* Version:      1.0
* Created:      2009-01-28-10.59.46
* Revision:     none
* Compiler:     gcc
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
#include <gtest/gtest.h>
#include <maidsafe/protobuf/kademlia_service_messages.pb.h>
#include <maidsafe/base/utils.h>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include "fs/filesystem.h"
#include "maidsafe/chunkstore.h"
#include "maidsafe/client/localstoremanager.h"
#include "maidsafe/client/sessionsingleton.h"
#include "protobuf/maidsafe_messages.pb.h"
#include "protobuf/maidsafe_service_messages.pb.h"
#include "tests/maidsafe/cached_keys.h"

namespace fs = boost::filesystem;

namespace test_lsm {

class FakeCallback {
 public:
  explicit FakeCallback(boost::mutex *m)
    : result_(), result(maidsafe::kGeneralError), mutex(m) {}
  void CallbackFunc(const std::string &res) {
    result_ = res;
  }
  void Reset() {
    result_ = "";
    result = maidsafe::kGeneralError;
  }
  std::string result_;
  maidsafe::ReturnCode result;
  boost::mutex *mutex;
};

void wait_for_result_lsm(const FakeCallback &cb, boost::mutex *mutex) {
  while (true) {
    {
      boost::mutex::scoped_lock guard(*mutex);
      if (cb.result_ != "") {
        return;
      }
    }
    boost::this_thread::sleep(boost::posix_time::milliseconds(5));
  }
}

void wait_for_result_lsm2(const FakeCallback &fcb, boost::mutex *mutex) {
  while (true) {
    {
      boost::mutex::scoped_lock guard(*mutex);
      if (fcb.result != maidsafe::kGeneralError) {
        return;
      }
    }
    boost::this_thread::sleep(boost::posix_time::milliseconds(5));
  }
}

void PacketOpCallback(const int &delete_result,
                      boost::mutex *mutex,
                      boost::condition_variable *cond_var,
                      int *result) {
  boost::mutex::scoped_lock lock(*mutex);
  *result = delete_result;
  cond_var->notify_all();
}

}  // namespace test_lsm

class LocalStoreManagerTest : public testing::Test {
 public:
  LocalStoreManagerTest() : test_root_dir_(file_system::TempDir() /
                                           ("maidsafe_TestStoreManager_" +
                                            base::RandomString(6))),
                            client_chunkstore_(),
                            sm_(),
                            co_(),
                            mutex_(),
                            cb(mutex_),
                            ss_(maidsafe::SessionSingleton::getInstance()),
                            keys_() {}
  ~LocalStoreManagerTest() {
    try {
      if (fs::exists(test_root_dir_))
        fs::remove_all(test_root_dir_);
      if (fs::exists(file_system::LocalStoreManagerDir()))
        fs::remove_all(file_system::LocalStoreManagerDir());
    }
    catch(const std::exception &e) {
      printf("%s\n", e.what());
    }
  }

 protected:
  void SetUp() {
    cached_keys::MakeKeys(3, &keys_);
    ss_->ResetSession();
    try {
      if (fs::exists(test_root_dir_))
        fs::remove_all(test_root_dir_);
      if (fs::exists(file_system::LocalStoreManagerDir()))
        fs::remove_all(file_system::LocalStoreManagerDir());
    }
    catch(const std::exception &e) {
      printf("%s\n", e.what());
    }
    mutex_ = new boost::mutex();
    client_chunkstore_ = boost::shared_ptr<maidsafe::ChunkStore>
         (new maidsafe::ChunkStore(test_root_dir_.string(), 0, 0));
    ASSERT_TRUE(client_chunkstore_->Init());
    int count(0);
    while (!client_chunkstore_->is_initialised() && count < 10000) {
      boost::this_thread::sleep(boost::posix_time::milliseconds(10));
      count += 10;
    }
    sm_ = new maidsafe::LocalStoreManager(client_chunkstore_);
    // sm_ = new Localsm_();
    sm_->Init(0, boost::bind(&test_lsm::FakeCallback::CallbackFunc, &cb, _1),
              test_root_dir_);
    wait_for_result_lsm(cb, mutex_);
    maidsafe::GenericResponse res;
    ASSERT_TRUE(res.ParseFromString(cb.result_));
    if (res.result() == kNack) {
      FAIL();
      return;
    }
    ss_ = maidsafe::SessionSingleton::getInstance();
    co_.set_symm_algorithm(crypto::AES_256);
    co_.set_hash_algorithm(crypto::SHA_512);
    ss_->AddKey(maidsafe::MPID, "Me", keys_.at(0).private_key(),
                keys_.at(0).public_key(), "");
    std::string anmid_pubkey_signature(co_.AsymSign(keys_.at(1).public_key(),
                                       "", keys_.at(1).private_key(),
                                       crypto::STRING_STRING));
    std::string anmid_name(co_.Hash(keys_.at(1).public_key() +
                           anmid_pubkey_signature, "", crypto::STRING_STRING,
                           false));
    ss_->AddKey(maidsafe::ANMID, anmid_name, keys_.at(1).private_key(),
                keys_.at(1).public_key(), anmid_pubkey_signature);
    cb.Reset();
  }

  void TearDown() {
    ss_->ResetSession();
    cb.Reset();
    sm_->Close(boost::bind(&test_lsm::FakeCallback::CallbackFunc,
                                    &cb, _1), true);
    wait_for_result_lsm(cb, mutex_);
    maidsafe::GenericResponse res;
    ASSERT_TRUE(res.ParseFromString(cb.result_));
    if (res.result() == kAck) {
      try {
        if (fs::exists(test_root_dir_))
          fs::remove_all(test_root_dir_);
        if (fs::exists(file_system::LocalStoreManagerDir()))
          fs::remove_all(file_system::LocalStoreManagerDir());
      }
      catch(const std::exception &e) {
        printf("%s\n", e.what());
      }
    }
  }

  fs::path test_root_dir_;
  boost::shared_ptr<maidsafe::ChunkStore> client_chunkstore_;
  maidsafe::LocalStoreManager *sm_;
  crypto::Crypto co_;
  boost::mutex *mutex_;
  test_lsm::FakeCallback cb;
  maidsafe::SessionSingleton *ss_;
  std::vector<crypto::RsaKeyPair> keys_;

 private:
  LocalStoreManagerTest(const LocalStoreManagerTest&);
  LocalStoreManagerTest &operator=(const LocalStoreManagerTest&);
};

TEST_F(LocalStoreManagerTest, BEH_MAID_RemoveAllPacketsFromKey) {
  kad::SignedValue gp;
  int result(maidsafe::kGeneralError);
  boost::mutex mutex;
  boost::condition_variable cond_var;
  std::string gp_name;

  // Store packets with same key, different values
  gp_name = co_.Hash("aaa", "", crypto::STRING_STRING, false);
  for (int i = 0; i < 5; ++i) {
    gp.set_value("Generic System Packet Data" +
                  boost::lexical_cast<std::string>(i));
    sm_->StorePacket(gp_name, gp.value(), maidsafe::MID,
        maidsafe::PRIVATE, "", maidsafe::kAppend, boost::bind(
        &test_lsm::PacketOpCallback, _1, &mutex, &cond_var, &result));
    {
      boost::mutex::scoped_lock lock(mutex);
      while (result == maidsafe::kGeneralError)
        cond_var.wait(lock);
    }
    ASSERT_EQ(maidsafe::kSuccess, result);
    result = maidsafe::kGeneralError;
  }

  // Remove said packets
  result = maidsafe::kGeneralError;
  sm_->DeletePacket(gp_name,
                    std::vector<std::string>(),  // Empty vector
                    maidsafe::MID,
                    maidsafe::PRIVATE,
                    "",
                    boost::bind(&test_lsm::PacketOpCallback,
                                _1,
                                &mutex,
                                &cond_var,
                                &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);

  // Ensure they're all gone
  ASSERT_TRUE(sm_->KeyUnique(gp_name, false));
}

TEST_F(LocalStoreManagerTest, BEH_MAID_StoreSystemPacket) {
  kad::SignedValue gp;
  gp.set_value("Generic System Packet Data");
  gp.set_value_signature(co_.AsymSign(gp.value(), "",
                         ss_->PrivateKey(maidsafe::ANMID),
                         crypto::STRING_STRING));
  std::string gp_name = co_.Hash(gp.value() + gp.value_signature(), "",
                        crypto::STRING_STRING, false);
  ASSERT_TRUE(sm_->KeyUnique(gp_name, false));
  int result(maidsafe::kGeneralError);
  boost::mutex mutex;
  boost::condition_variable cond_var;
  sm_->StorePacket(gp_name, gp.value(), maidsafe::MID,
      maidsafe::PRIVATE, "", maidsafe::kDoNothingReturnFailure, boost::bind(
      &test_lsm::PacketOpCallback, _1, &mutex, &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);
  ASSERT_FALSE(sm_->KeyUnique(gp_name, false));
  std::vector<std::string> res;
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(size_t(1), res.size());
  kad::SignedValue gp_res;
  ASSERT_TRUE(gp_res.ParseFromString(res[0]));
  ASSERT_EQ(gp.value(), gp_res.value());
  ASSERT_EQ(gp.value_signature(), gp_res.value_signature());
}

TEST_F(LocalStoreManagerTest, BEH_MAID_DeleteSystemPacketOwner) {
  kad::SignedValue gp;
  gp.set_value("Generic System Packet Data");
  gp.set_value_signature(co_.AsymSign(gp.value(), "",
                         ss_->PrivateKey(maidsafe::ANMID),
                         crypto::STRING_STRING));
  std::string gp_name = co_.Hash(gp.value() + gp.value_signature(), "",
                        crypto::STRING_STRING, false);

  std::string signed_public_key = co_.AsymSign(keys_.at(2).public_key(), "",
                                  keys_.at(2).private_key(),
                                  crypto::STRING_STRING);
  int result(maidsafe::kGeneralError);
  boost::mutex mutex;
  boost::condition_variable cond_var;
  sm_->StorePacket(gp_name, gp.value(), maidsafe::MID, maidsafe::PRIVATE, "",
                   maidsafe::kDoNothingReturnFailure, boost::bind(
                   &test_lsm::PacketOpCallback, _1, &mutex, &cond_var,
                   &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);

  ASSERT_FALSE(sm_->KeyUnique(gp_name, false));

  result = maidsafe::kGeneralError;
  std::vector<std::string> values(1, gp.value());
  sm_->DeletePacket(gp_name, values, maidsafe::MID, maidsafe::PRIVATE, "",
                    boost::bind(&test_lsm::PacketOpCallback, _1, &mutex,
                    &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);

  ASSERT_TRUE(sm_->KeyUnique(gp_name, false));
}

TEST_F(LocalStoreManagerTest, BEH_MAID_DeleteSystemPacketNotOwner) {
  kad::SignedValue gp;
  gp.set_value("Generic System Packet Data");
  gp.set_value_signature(co_.AsymSign(gp.value(), "",
                         ss_->PrivateKey(maidsafe::ANMID),
                         crypto::STRING_STRING));
  std::string gp_name = co_.Hash(gp.value() + gp.value_signature(), "",
                        crypto::STRING_STRING, false);

  int result(maidsafe::kGeneralError);
  boost::mutex mutex;
  boost::condition_variable cond_var;
  sm_->StorePacket(gp_name, gp.value(), maidsafe::MID, maidsafe::PRIVATE, "",
                   maidsafe::kDoNothingReturnFailure, boost::bind(
                   &test_lsm::PacketOpCallback, _1, &mutex, &cond_var,
                   &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);
  ASSERT_FALSE(sm_->KeyUnique(gp_name, false));

  result = maidsafe::kGeneralError;
  std::vector<std::string> values(1, gp.value());

  std::string anmid_pubkey_signature(co_.AsymSign(keys_.at(2).public_key(),
                                     "", keys_.at(2).private_key(),
                                     crypto::STRING_STRING));
  std::string anmid_name(co_.Hash(keys_.at(2).public_key() +
                         anmid_pubkey_signature, "", crypto::STRING_STRING,
                         false));
  ss_->AddKey(maidsafe::ANMID, anmid_name, keys_.at(2).private_key(),
              keys_.at(2).public_key(), anmid_pubkey_signature);

  sm_->DeletePacket(gp_name, values, maidsafe::MID, maidsafe::PRIVATE, "",
                    boost::bind(&test_lsm::PacketOpCallback, _1, &mutex,
                    &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_FALSE(sm_->KeyUnique(gp_name, false));
}

TEST_F(LocalStoreManagerTest, BEH_MAID_UpdatePacket) {
  // Store one packet
  kad::SignedValue gp;
  gp.set_value("Generic System Packet Data");
  gp.set_value_signature(co_.AsymSign(gp.value(), "",
                                      ss_->PrivateKey(maidsafe::ANMID),
                                      crypto::STRING_STRING));
  std::string gp_name(co_.Hash(gp.value() + gp.value_signature(), "",
                      crypto::STRING_STRING, false));

  int result(maidsafe::kGeneralError);
  boost::mutex mutex;
  boost::condition_variable cond_var;
  sm_->StorePacket(gp_name, gp.value(), maidsafe::MID, maidsafe::PRIVATE, "",
                   maidsafe::kDoNothingReturnFailure,
                   boost::bind(&test_lsm::PacketOpCallback, _1, &mutex,
                               &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);
  std::vector<std::string> res;
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(size_t(1), res.size());
  ASSERT_EQ(gp.SerializeAsString(), res[0]);

  // Update the packet
  result = maidsafe::kGeneralError;
  kad::SignedValue new_gp;
  new_gp.set_value("Mis bolas enormes y peludas");
  new_gp.set_value_signature(co_.AsymSign(new_gp.value(), "",
                                          ss_->PrivateKey(maidsafe::ANMID),
                                          crypto::STRING_STRING));
  sm_->UpdatePacket(gp_name, gp.value(), new_gp.value(), maidsafe::MID,
                    maidsafe::PRIVATE, "",
                    boost::bind(&test_lsm::PacketOpCallback, _1, &mutex,
                                &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);
  res.clear();
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(size_t(1), res.size());
  ASSERT_EQ(new_gp.SerializeAsString(), res[0]);

  // Store another value with that same key
  result = maidsafe::kGeneralError;
  gp.set_value("Mira nada mas que chichotas");
  gp.set_value_signature(co_.AsymSign(gp.value(), "",
                                      ss_->PrivateKey(maidsafe::ANMID),
                                      crypto::STRING_STRING));
  sm_->StorePacket(gp_name, gp.value(), maidsafe::MID, maidsafe::PRIVATE, "",
                   maidsafe::kAppend, boost::bind(&test_lsm::PacketOpCallback,
                                                  _1, &mutex, &cond_var,
                                                  &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);
  res.clear();
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(size_t(2), res.size());
  for (size_t n = 0; n < res.size(); ++n)
    ASSERT_TRUE(res[n] == gp.SerializeAsString() ||
                res[n] == new_gp.SerializeAsString());

  // Change one of the values
  kad::SignedValue other_gp = gp;
  gp.set_value("En esa cola si me formo");
  gp.set_value_signature(co_.AsymSign(gp.value(), "",
                                      ss_->PrivateKey(maidsafe::ANMID),
                                      crypto::STRING_STRING));
  sm_->UpdatePacket(gp_name, new_gp.value(), gp.value(), maidsafe::MID,
                    maidsafe::PRIVATE, "",
                    boost::bind(&test_lsm::PacketOpCallback, _1, &mutex,
                                &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kSuccess, result);
  res.clear();
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(size_t(2), res.size());
  std::set<std::string> all_values;
  for (size_t n = 0; n < res.size(); ++n) {
    ASSERT_TRUE(res[n] == gp.SerializeAsString() ||
                res[n] == other_gp.SerializeAsString()) << n;
    all_values.insert(res[n]);
  }

  // Store several values with that same key
  for (size_t a = 0; a < 5; ++a) {
    result = maidsafe::kGeneralError;
    gp.set_value("value" + base::IntToString(a));
    gp.set_value_signature(co_.AsymSign(gp.value(), "",
                                        ss_->PrivateKey(maidsafe::ANMID),
                                        crypto::STRING_STRING));
    sm_->StorePacket(gp_name, gp.value(), maidsafe::MID, maidsafe::PRIVATE, "",
                     maidsafe::kAppend, boost::bind(&test_lsm::PacketOpCallback,
                                                    _1, &mutex, &cond_var,
                                                    &result));
    {
      boost::mutex::scoped_lock lock(mutex);
      while (result == maidsafe::kGeneralError)
        cond_var.wait(lock);
    }
    ASSERT_EQ(maidsafe::kSuccess, result);
    all_values.insert(gp.SerializeAsString());
  }
  res.clear();
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(all_values.size(), res.size());
  std::set<std::string>::iterator it;
  for (size_t n = 0; n < res.size(); ++n) {
    it = all_values.find(res[n]);
    ASSERT_FALSE(it == all_values.end());
  }

  // Try to change one of the values to another one
  result = maidsafe::kGeneralError;
  sm_->UpdatePacket(gp_name, "value0", "value2", maidsafe::MID,
                    maidsafe::PRIVATE, "",
                    boost::bind(&test_lsm::PacketOpCallback, _1, &mutex,
                                &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kStoreManagerError, result);
  res.clear();
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(all_values.size(), res.size());
  for (size_t n = 0; n < res.size(); ++n) {
    it = all_values.find(res[n]);
    ASSERT_FALSE(it == all_values.end());
  }

  // Try to update with different keys
  crypto::RsaKeyPair rkp;
  rkp.GenerateKeys(4096);
  std::string anmid_pubkey_signature(co_.AsymSign(rkp.public_key(), "",
                                                  rkp.private_key(),
                                                  crypto::STRING_STRING));
  std::string anmid_name(co_.Hash(rkp.public_key() + anmid_pubkey_signature, "",
                                  crypto::STRING_STRING, false));
  ss_->AddKey(maidsafe::ANMID, anmid_name, rkp.private_key(),
              rkp.public_key(), anmid_pubkey_signature);
  result = maidsafe::kGeneralError;
  sm_->UpdatePacket(gp_name, "value0", "value1234", maidsafe::MID,
                    maidsafe::PRIVATE, "",
                    boost::bind(&test_lsm::PacketOpCallback, _1, &mutex,
                                &cond_var, &result));
  {
    boost::mutex::scoped_lock lock(mutex);
    while (result == maidsafe::kGeneralError)
      cond_var.wait(lock);
  }
  ASSERT_EQ(maidsafe::kStoreManagerError, result);
  res.clear();
  ASSERT_EQ(maidsafe::kSuccess, sm_->LoadPacket(gp_name, &res));
  ASSERT_EQ(all_values.size(), res.size());
  for (size_t n = 0; n < res.size(); ++n) {
    it = all_values.find(res[n]);
    ASSERT_FALSE(it == all_values.end());
  }
  all_values = std::set<std::string>(res.begin(), res.end());
  it = all_values.find("value1234");
  ASSERT_TRUE(it == all_values.end());
}

TEST_F(LocalStoreManagerTest, BEH_MAID_StoreChunk) {
  std::string chunk_content = base::RandomString(256 * 1024);
  std::string chunk_name = co_.Hash(chunk_content, "",
                           crypto::STRING_STRING, false);
  std::string hex_chunk_name = base::EncodeToHex(chunk_name);
  fs::path chunk_path(test_root_dir_ / hex_chunk_name);
  fs::ofstream ofs;
  ofs.open(chunk_path.string().c_str());
  ofs << chunk_content;
  ofs.close();
  client_chunkstore_->AddChunkToOutgoing(chunk_name, chunk_path);
  ASSERT_TRUE(sm_->KeyUnique(chunk_name, false));
  sm_->StoreChunk(chunk_name, maidsafe::PRIVATE, "");

  ASSERT_FALSE(sm_->KeyUnique(chunk_name, false));
  std::string result_str;
  ASSERT_EQ(0, sm_->LoadChunk(chunk_name, &result_str));
  ASSERT_EQ(chunk_content, result_str);
}

TEST_F(LocalStoreManagerTest, BEH_MAID_StoreAndLoadBufferPacket) {
  std::string bufferpacketname = co_.Hash(ss_->Id(maidsafe::MPID) +
                                 ss_->PublicKey(maidsafe::MPID), "",
                                 crypto::STRING_STRING, false);
  ASSERT_TRUE(sm_->KeyUnique(bufferpacketname, false));
  ASSERT_EQ(0, sm_->CreateBP());
  ASSERT_FALSE(sm_->KeyUnique(bufferpacketname, false));
  std::string packet_content;
  sm_->LoadChunk(bufferpacketname, &packet_content);
  maidsafe::BufferPacket buffer_packet;
  ASSERT_TRUE(buffer_packet.ParseFromString(packet_content));
  ASSERT_EQ(1, buffer_packet.owner_info_size());
  ASSERT_EQ(0, buffer_packet.messages_size());
  maidsafe::GenericPacket gp = buffer_packet.owner_info(0);
  ASSERT_TRUE(co_.AsymCheckSig(gp.data(), gp.signature(),
              ss_->PublicKey(maidsafe::MPID), crypto::STRING_STRING));
  maidsafe::BufferPacketInfo bpi;
  ASSERT_TRUE(bpi.ParseFromString(gp.data()));
  ASSERT_EQ(ss_->Id(maidsafe::MPID), bpi.owner());
  ASSERT_EQ(ss_->PublicKey(maidsafe::MPID), bpi.owner_publickey());
}

TEST_F(LocalStoreManagerTest, BEH_MAID_ModifyBufferPacketInfo) {
  std::string bufferpacketname = co_.Hash(ss_->Id(maidsafe::MPID) +
                                 ss_->PublicKey(maidsafe::MPID), "",
                                 crypto::STRING_STRING, false);
  ASSERT_TRUE(sm_->KeyUnique(bufferpacketname, false));
  ASSERT_EQ(0, sm_->CreateBP());
  ASSERT_FALSE(sm_->KeyUnique(bufferpacketname, false));
  std::string packet_content;
  sm_->LoadChunk(bufferpacketname, &packet_content);
  maidsafe::BufferPacket buffer_packet;
  ASSERT_TRUE(buffer_packet.ParseFromString(packet_content));
  ASSERT_EQ(1, buffer_packet.owner_info_size());
  ASSERT_EQ(0, buffer_packet.messages_size());
  maidsafe::GenericPacket gp = buffer_packet.owner_info(0);
  ASSERT_TRUE(co_.AsymCheckSig(gp.data(), gp.signature(),
              ss_->PublicKey(maidsafe::MPID), crypto::STRING_STRING));
  maidsafe::BufferPacketInfo bpi;
  ASSERT_TRUE(bpi.ParseFromString(gp.data()));
  ASSERT_EQ(ss_->Id(maidsafe::MPID), bpi.owner());
  ASSERT_EQ(ss_->PublicKey(maidsafe::MPID), bpi.owner_publickey());

  // Modifying the BP info
  maidsafe::BufferPacketInfo buffer_packet_info;
  std::string ser_info;
  buffer_packet_info.set_owner(ss_->Id(maidsafe::MPID));
  buffer_packet_info.set_owner_publickey(ss_->PublicKey(maidsafe::MPID));
  buffer_packet_info.add_users("Juanito");
  buffer_packet_info.SerializeToString(&ser_info);
  ASSERT_EQ(0, sm_->ModifyBPInfo(ser_info));
  packet_content = "";
  ASSERT_EQ(0, sm_->LoadChunk(bufferpacketname, &packet_content));
  ASSERT_TRUE(buffer_packet.ParseFromString(packet_content));
  gp = buffer_packet.owner_info(0);
  std::string ser_gp;
  ASSERT_TRUE(gp.SerializeToString(&ser_gp));
  ASSERT_TRUE(co_.AsymCheckSig(gp.data(), gp.signature(),
              ss_->PublicKey(maidsafe::MPID), crypto::STRING_STRING));
  ASSERT_EQ(ser_info, gp.data());
}

TEST_F(LocalStoreManagerTest, BEH_MAID_AddAndGetBufferPacketMessages) {
  std::string bufferpacketname = co_.Hash(ss_->Id(maidsafe::MPID) +
                                 ss_->PublicKey(maidsafe::MPID), "",
                                 crypto::STRING_STRING, false);
  ASSERT_TRUE(sm_->KeyUnique(bufferpacketname, false));
  ASSERT_EQ(0, sm_->CreateBP());
  ASSERT_FALSE(sm_->KeyUnique(bufferpacketname, false));
  maidsafe::BufferPacketInfo buffer_packet_info;
  buffer_packet_info.set_owner(ss_->Id(maidsafe::MPID));
  buffer_packet_info.set_owner_publickey(ss_->PublicKey(maidsafe::MPID));
  buffer_packet_info.add_users(co_.Hash("Juanito", "",
                               crypto::STRING_STRING, false));
  std::string ser_info;
  buffer_packet_info.SerializeToString(&ser_info);
  ASSERT_EQ(0, sm_->ModifyBPInfo(ser_info));

  std::string me_pubusername = ss_->Id(maidsafe::MPID);
  std::string me_pubkey = ss_->PublicKey(maidsafe::MPID);
  std::string me_privkey = ss_->PrivateKey(maidsafe::MPID);
  std::string me_sigpubkey = co_.AsymSign(me_pubkey, "",
                             me_privkey, crypto::STRING_STRING);

  // Create the user "Juanito", add to session, then the message and send it
  ss_->ResetSession();
  std::string private_key = keys_.at(2).private_key();
  std::string public_key = keys_.at(2).public_key();
  std::string signed_public_key = co_.AsymSign(public_key, "",
                                  private_key, crypto::STRING_STRING);
  ss_->AddKey(maidsafe::MPID, "Juanito", private_key, public_key,
              signed_public_key);
  ASSERT_EQ(0, ss_->AddContact(me_pubusername, me_pubkey, "", "", "", 'U', 1, 2,
            "", 'C', 0, 0));

  std::vector<std::string> pulbicusernames;
  std::string test_msg("There are strange things done in the midnight sun");
  pulbicusernames.push_back(me_pubusername);
  std::map<std::string, maidsafe::ReturnCode> add_results;
  ASSERT_EQ(static_cast<int>(pulbicusernames.size()),
            sm_->SendMessage(pulbicusernames, test_msg, maidsafe::INSTANT_MSG,
                              &add_results));

  // Retrive message and check that it is the correct one
  ss_->ResetSession();
  ss_->AddKey(maidsafe::MPID, me_pubusername, me_privkey, me_pubkey,
              me_sigpubkey);
  std::list<maidsafe::ValidatedBufferPacketMessage> messages;
  ASSERT_EQ(kKadUpperThreshold, sm_->LoadBPMessages(&messages));
  ASSERT_EQ(size_t(1), messages.size());
  ASSERT_EQ("Juanito", messages.front().sender());
  ASSERT_EQ(test_msg, messages.front().message());
  ASSERT_EQ("", messages.front().index());
  ASSERT_EQ(maidsafe::INSTANT_MSG, messages.front().type());

  // Check message is gone
  ASSERT_EQ(kKadUpperThreshold, sm_->LoadBPMessages(&messages));
  ASSERT_EQ(size_t(0), messages.size());
}

TEST_F(LocalStoreManagerTest, BEH_MAID_AddRequestBufferPacketMessage) {
  std::string bufferpacketname = co_.Hash(ss_->Id(maidsafe::MPID) +
                                 ss_->PublicKey(maidsafe::MPID), "",
                                 crypto::STRING_STRING, false);
  ASSERT_TRUE(sm_->KeyUnique(bufferpacketname, false));
  ASSERT_EQ(0, sm_->CreateBP());
  ASSERT_FALSE(sm_->KeyUnique(bufferpacketname, false));
  std::string me_pubusername = ss_->Id(maidsafe::MPID);
  std::string me_pubkey = ss_->PublicKey(maidsafe::MPID);
  std::string me_privkey = ss_->PrivateKey(maidsafe::MPID);
  std::string me_sigpubkey = co_.AsymSign(me_pubkey, "",
                             me_privkey, crypto::STRING_STRING);

  // Create the user "Juanito", add to session, then the message and send it
  ss_->ResetSession();
  std::string private_key = keys_.at(2).private_key();
  std::string public_key = keys_.at(2).public_key();
  std::string signed_public_key = co_.AsymSign(public_key, "",
                                  private_key, crypto::STRING_STRING);
  ss_->AddKey(maidsafe::MPID, "Juanito", private_key, public_key,
              signed_public_key);
  ASSERT_EQ(0, ss_->AddContact(me_pubusername, me_pubkey, "", "", "", 'U', 1, 2,
            "", 'C', 0, 0));
  std::vector<std::string> pulbicusernames;
  std::string test_msg("There are strange things done in the midnight sun");
  pulbicusernames.push_back(me_pubusername);
  std::map<std::string, maidsafe::ReturnCode> add_results;
  ASSERT_EQ(0, sm_->SendMessage(pulbicusernames, test_msg,
            maidsafe::INSTANT_MSG, &add_results));
  add_results.clear();
  ASSERT_EQ(static_cast<int>(pulbicusernames.size()),
            sm_->SendMessage(pulbicusernames, test_msg,
                              maidsafe::ADD_CONTACT_RQST, &add_results));

  // Back to "Me"
  ss_->ResetSession();
  ss_->AddKey(maidsafe::MPID, me_pubusername, me_privkey, me_pubkey,
              me_sigpubkey);
  std::list<maidsafe::ValidatedBufferPacketMessage> messages;
  ASSERT_EQ(kKadUpperThreshold, sm_->LoadBPMessages(&messages));
  ASSERT_EQ(size_t(1), messages.size());
  ASSERT_EQ("Juanito", messages.front().sender());
  ASSERT_EQ(test_msg, messages.front().message());
  ASSERT_EQ("", messages.front().index());
  ASSERT_EQ(maidsafe::ADD_CONTACT_RQST, messages.front().type());
  maidsafe::BufferPacketInfo buffer_packet_info;
  buffer_packet_info.set_owner(ss_->Id(maidsafe::MPID));
  buffer_packet_info.set_owner_publickey(ss_->PublicKey(maidsafe::MPID));
  buffer_packet_info.add_users(co_.Hash("Juanito", "",
                               crypto::STRING_STRING, false));
  std::string ser_info;
  buffer_packet_info.SerializeToString(&ser_info);
  ASSERT_EQ(0, sm_->ModifyBPInfo(ser_info));

  // Back to "Juanito"
  ss_->ResetSession();
  ss_->AddKey(maidsafe::MPID, "Juanito", private_key, public_key,
              signed_public_key);
  ASSERT_EQ(0, ss_->AddContact(me_pubusername, me_pubkey, "", "", "", 'U', 1, 2,
            "", 'C', 0, 0));
  pulbicusernames.clear();
  pulbicusernames.push_back(me_pubusername);
  add_results.clear();
  ASSERT_EQ(static_cast<int>(pulbicusernames.size()),
            sm_->SendMessage(pulbicusernames, test_msg, maidsafe::INSTANT_MSG,
                              &add_results));

  // Back to "Me"
  ss_->ResetSession();
  ss_->AddKey(maidsafe::MPID, me_pubusername, me_privkey, me_pubkey,
              me_sigpubkey);
  messages.clear();
  ASSERT_EQ(kKadUpperThreshold, sm_->LoadBPMessages(&messages));
  ASSERT_EQ(size_t(1), messages.size());
  ASSERT_EQ("Juanito", messages.front().sender());
  ASSERT_EQ(test_msg, messages.front().message());
  ASSERT_EQ("", messages.front().index());
  ASSERT_EQ(maidsafe::INSTANT_MSG, messages.front().type());
}
