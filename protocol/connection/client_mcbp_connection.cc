/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "client_mcbp_connection.h"

#include <array>
#include <cbsasl/cbsasl.h>
#include <engines/ewouldblock_engine/ewouldblock_engine.h>
#include <extensions/protocol/testapp_extension.h>
#include <iostream>
#include <iterator>
#include <libmcbp/mcbp.h>
#include <memcached/protocol_binary.h>
#include <platform/strerror.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <include/memcached/protocol_binary.h>

static const bool packet_dump = getenv("COUCHBASE_PACKET_DUMP") != nullptr;

static void mcbp_raw_command(Frame& frame,
                             uint8_t cmd,
                             const std::vector<uint8_t>& ext,
                             const std::string& key,
                             const std::vector<uint8_t>& value,
                             uint64_t cas = 0,
                             uint32_t opaque = 0xdeadbeef) {

    auto& pay = frame.payload;
    pay.resize(24 + ext.size() + key.size() + value.size());
    auto* req = reinterpret_cast<protocol_binary_request_header*>(pay.data());
    auto* buf = req->bytes;

    req->request.magic = PROTOCOL_BINARY_REQ;
    req->request.opcode = cmd;
    req->request.extlen = static_cast<uint8_t>(ext.size());
    req->request.keylen = htons(static_cast<uint16_t>(key.size()));
    auto bodylen = value.size() + key.size() + ext.size();
    req->request.bodylen = htonl(static_cast<uint32_t>(bodylen));
    req->request.opaque = opaque;
    req->request.cas = cas;

    buf += sizeof(req->bytes);
    memcpy(buf, ext.data(), ext.size());
    buf += ext.size();
    memcpy(buf, key.data(), key.size());
    buf += key.size();
    memcpy(buf, value.data(), value.size());
}

static void mcbp_storage_command(Frame& frame,
                                 uint8_t cmd,
                                 const std::string& id,
                                 const std::vector<uint8_t>& value,
                                 uint32_t flags,
                                 uint32_t exp) {
    frame.reset();
    std::vector<uint8_t> ext;


    if (cmd != PROTOCOL_BINARY_CMD_APPEND &&
        cmd != PROTOCOL_BINARY_CMD_PREPEND) {
        uint32_t fl = htonl(flags);
        uint32_t expiry = htonl(exp);
        ext.resize(8);
        memcpy(ext.data(), &fl, sizeof(fl));
        memcpy(ext.data() + sizeof(fl), &expiry, sizeof(expiry));
    }

    mcbp_raw_command(frame, cmd, ext, id, value);
}

/////////////////////////////////////////////////////////////////////////
// SASL related functions
/////////////////////////////////////////////////////////////////////////
struct my_sasl_ctx {
    const char* username;
    cbsasl_secret_t* secret;
};

static int sasl_get_username(void* context, int id, const char** result,
                             unsigned int* len) {
    struct my_sasl_ctx* ctx = reinterpret_cast<struct my_sasl_ctx*>(context);
    if (!context || !result ||
        (id != CBSASL_CB_USER && id != CBSASL_CB_AUTHNAME)) {
        return CBSASL_BADPARAM;
    }

    *result = ctx->username;
    if (len) {
        *len = (unsigned int)strlen(*result);
    }

    return CBSASL_OK;
}

static int sasl_get_password(cbsasl_conn_t* conn, void* context, int id,
                             cbsasl_secret_t** psecret) {
    struct my_sasl_ctx* ctx = reinterpret_cast<struct my_sasl_ctx*>(context);
    if (!conn || !psecret || id != CBSASL_CB_PASS || ctx == NULL) {
        return CBSASL_BADPARAM;
    }

    *psecret = ctx->secret;
    return CBSASL_OK;
}

/////////////////////////////////////////////////////////////////////////
// Implementation of the MemcachedBinaryConnection class
/////////////////////////////////////////////////////////////////////////

std::unique_ptr<MemcachedConnection> MemcachedBinprotConnection::clone() {
    auto* result = new MemcachedBinprotConnection(this->host,
                                                  this->port,
                                                  this->family,
                                                  this->ssl);
    return std::unique_ptr<MemcachedConnection>{result};
}

void MemcachedBinprotConnection::sendFrame(const Frame& frame) {
    MemcachedConnection::sendFrame(frame);
    if (packet_dump) {
        Couchbase::MCBP::dump(frame.payload.data(), std::cerr);
    }
}


void MemcachedBinprotConnection::recvFrame(Frame& frame) {
    frame.reset();
    // A memcached packet starts with a 24 byte fixed header
    MemcachedConnection::read(frame, 24);

    // Following the header is the full payload specified in the field
    // bodylen. Luckily for us the bodylen is located at the same offset in
    // both a request and a response message..
    auto* req = reinterpret_cast<protocol_binary_request_header*>(frame.payload.data());
    const uint32_t bodylen = ntohl(req->request.bodylen);
    const uint8_t magic = frame.payload.at(0);
    const uint8_t REQUEST = uint8_t(PROTOCOL_BINARY_REQ);
    const uint8_t RESPONSE = uint8_t(PROTOCOL_BINARY_RES);

    if (magic != REQUEST && magic != RESPONSE) {
        throw std::runtime_error("Invalid magic received: " +
                                 std::to_string(magic));
    }

    MemcachedConnection::read(frame, bodylen);
    if (packet_dump) {
        Couchbase::MCBP::dump(frame.payload.data(), std::cerr);
    }

    // fixup the length bits in the header to be in host local order:
    if (magic == REQUEST) {
        // The underlying buffer may hage been reallocated as part of read
        req = reinterpret_cast<protocol_binary_request_header*>(frame.payload.data());
        req->request.keylen = ntohs(req->request.keylen);
        req->request.bodylen = bodylen;
    } else {
        // The underlying buffer may hage been reallocated as part of read
        auto* res = reinterpret_cast<protocol_binary_response_header*>(frame.payload.data());
        res->response.keylen = ntohs(res->response.keylen);
        res->response.bodylen = bodylen;
        res->response.status = ntohs(res->response.status);
    }
}

void MemcachedBinprotConnection::authenticate(const std::string& username,
                                              const std::string& password,
                                              const std::string& mech) {
    cbsasl_error_t err;
    const char* data;
    unsigned int len;
    const char* chosenmech;
    struct my_sasl_ctx context;
    cbsasl_callback_t sasl_callbacks[4];
    cbsasl_conn_t* client;

    sasl_callbacks[0].id = CBSASL_CB_USER;
    sasl_callbacks[0].proc = (int (*)(void))&sasl_get_username;
    sasl_callbacks[0].context = &context;
    sasl_callbacks[1].id = CBSASL_CB_AUTHNAME;
    sasl_callbacks[1].proc = (int (*)(void))&sasl_get_username;
    sasl_callbacks[1].context = &context;
    sasl_callbacks[2].id = CBSASL_CB_PASS;
    sasl_callbacks[2].proc = (int (*)(void))&sasl_get_password;
    sasl_callbacks[2].context = &context;
    sasl_callbacks[3].id = CBSASL_CB_LIST_END;
    sasl_callbacks[3].proc = NULL;
    sasl_callbacks[3].context = NULL;

    context.username = username.c_str();
    std::vector<uint8_t> buffer(
        sizeof(context.secret->len) + password.length() + 10);
    context.secret = reinterpret_cast<cbsasl_secret_t*>(buffer.data());
    memcpy(context.secret->data, password.c_str(), password.length());
    context.secret->len = password.length();

    err = cbsasl_client_new(NULL, NULL, NULL, NULL, sasl_callbacks, 0, &client);
    if (err != CBSASL_OK) {
        throw std::runtime_error(
            std::string("cbsasl_client_new: ") + std::to_string(err));
    }
    err = cbsasl_client_start(client, mech.c_str(), NULL, &data, &len,
                              &chosenmech);
    if (err != CBSASL_OK) {
        throw std::runtime_error(
            std::string("cbsasl_client_start (") + std::string(chosenmech) +
            std::string("): ") + std::to_string(err));
    }

    Frame request;

    std::vector<uint8_t> challenge(len);
    memcpy(challenge.data(), data, len);
    const std::string mechanism(chosenmech);
    mcbp_raw_command(request, PROTOCOL_BINARY_CMD_SASL_AUTH,
                     std::vector<uint8_t>(), mechanism, challenge);

    sendFrame(request);
    Frame response;
    recvFrame(response);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(response.payload.data());

    while (rsp->message.header.response.status ==
           PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE) {
        int datalen = rsp->message.header.response.bodylen -
                      rsp->message.header.response.keylen -
                      rsp->message.header.response.extlen;

        int dataoffset = sizeof(rsp->bytes) +
                         rsp->message.header.response.keylen +
                         rsp->message.header.response.extlen;

        err = cbsasl_client_step(client,
                                 reinterpret_cast<char*>(rsp->bytes +
                                                         dataoffset),
                                 datalen,
                                 NULL, &data, &len);
        if (err != CBSASL_OK && err != CBSASL_CONTINUE) {
            reconnect();
            throw std::runtime_error(
                std::string("cbsasl_client_step: ") + std::to_string(err));
        }
        request.reset();

        challenge.resize(len);
        memcpy(challenge.data(), data, len);
        mcbp_raw_command(request, PROTOCOL_BINARY_CMD_SASL_STEP,
                         std::vector<uint8_t>(), mechanism, challenge);

        sendFrame(request);
        recvFrame(response);
        rsp = reinterpret_cast<protocol_binary_response_no_extras*>(response.payload.data());
    }

    cbsasl_dispose(&client);

    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Authentication failed: ",
                                     rsp->message.header.response.status);
    }
}

void MemcachedBinprotConnection::createBucket(const std::string& name,
                                              const std::string& config,
                                              const Greenstack::BucketType& type) {
    std::string module;
    switch (type) {
    case Greenstack::BucketType::Memcached:
        module.assign("default_engine.so");
        break;
    case Greenstack::BucketType::EWouldBlock:
        module.assign("ewouldblock_engine.so");
        break;
    case Greenstack::BucketType::Couchbase:
        module.assign("ep.so");
        break;
    default:
        throw std::runtime_error("Not implemented");
    }

    std::vector<uint8_t> payload;
    payload.resize(module.length() + 1 + config.length());
    memcpy(payload.data(), module.data(), module.length());
    memcpy(payload.data() + module.length() + 1, config.data(),
           config.length());

    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_CREATE_BUCKET,
                     std::vector<uint8_t>(), name, payload);

    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());

    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Create bucket failed: ",
                                     rsp->message.header.response.status);
    }
}

void MemcachedBinprotConnection::deleteBucket(const std::string& name) {
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_DELETE_BUCKET,
                     std::vector<uint8_t>(), name, std::vector<uint8_t>());
    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());

    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Delete bucket failed: ",
                                     rsp->message.header.response.status);
    }
}

void MemcachedBinprotConnection::selectBucket(const std::string& name) {
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_SELECT_BUCKET,
                     std::vector<uint8_t>(), name, std::vector<uint8_t>());
    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());

    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Select bucket failed: ",
                                     rsp->message.header.response.status);
    }
}

std::string MemcachedBinprotConnection::to_string() {
    std::string ret("Memcached connection ");
    ret.append(std::to_string(port));
    if (family == AF_INET6) {
        ret.append("[::!]:");
    } else {
        ret.append("127.0.0.1:");
    }

    ret.append(std::to_string(port));

    if (ssl) {
        ret.append(" ssl");
    }

    return ret;
}

std::vector<std::string> MemcachedBinprotConnection::listBuckets() {
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_LIST_BUCKETS,
                     std::vector<uint8_t>(), "", std::vector<uint8_t>());
    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());

    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("List bucket failed: ",
                                     rsp->message.header.response.status);
    }

    std::vector<std::string> ret;
    std::string value((char*)(rsp + 1), rsp->message.header.response.bodylen);
    // the value contains a list of bucket names separated by space.
    std::istringstream iss(value);
    std::copy(std::istream_iterator<std::string>(iss),
              std::istream_iterator<std::string>(),
              std::back_inserter(ret));

    return ret;
}

Document MemcachedBinprotConnection::get(const std::string& id,
                                         uint16_t vbucket) {
    Frame frame = encodeCmdGet(id, vbucket);
    sendFrame(frame);

    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_get*>(frame.payload.data());

    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Failed to get: " + id,
                                     rsp->message.header.response.status);
    }

    Document ret;
    ret.info.flags = ntohl(rsp->message.body.flags);
    ret.info.cas = rsp->message.header.response.cas;
    ret.info.id = id;
    if (rsp->message.header.response.datatype & PROTOCOL_BINARY_DATATYPE_JSON) {
        ret.info.datatype = Greenstack::Datatype::Json;
    } else {
        ret.info.datatype = Greenstack::Datatype::Raw;
    }

    if (rsp->message.header.response.datatype &
        PROTOCOL_BINARY_DATATYPE_COMPRESSED) {
        ret.info.compression = Greenstack::Compression::Snappy;
    } else {
        ret.info.compression = Greenstack::Compression::None;
    }

    ret.value.resize(rsp->message.header.response.bodylen - 4);
    memcpy(ret.value.data(), rsp->bytes + 28, ret.value.size());

    return ret;
}

Frame MemcachedBinprotConnection::encodeCmdGet(const std::string& id,
                                               uint16_t vbucket) {
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_GET, std::vector<uint8_t>(), id,
                     std::vector<uint8_t>());
    auto* req =
            reinterpret_cast<protocol_binary_request_no_extras*>(frame.payload.data());
    req->message.header.request.vbucket = htons(vbucket);
    return frame;
}

/* Convenience function which will insert (copy) T into the given container. Only
 * safe if T is trivially copyable (i.e. save to use memcpy on).
 */
template<typename T>
void encode_to(std::vector<uint8_t>& container, const T& element) {
    const auto* elem_ptr = reinterpret_cast<const uint8_t*>(&element);
    container.insert(container.end(), elem_ptr, elem_ptr + sizeof(element));
}

Frame MemcachedBinprotConnection::encodeCmdDcpOpen() {
    Frame frame;

    // Encode extras
    std::vector<uint8_t> extras;
    encode_to(extras, htonl(0));
    encode_to(extras, uint32_t{DCP_OPEN_PRODUCER});

    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_DCP_OPEN, extras,
                     /*key*/"dcp", /*value*/{});

    return frame;
}

Frame MemcachedBinprotConnection::encodeCmdDcpStreamReq() {
    Frame frame;

    // Encode extras
    std::vector<uint8_t> extras;
    encode_to(extras, htonl(0));  // flags
    encode_to(extras, uint32_t{});  // reserved
    encode_to(extras, htonll(std::numeric_limits<uint64_t>::min()));  // start_seqno
    encode_to(extras, htonll(std::numeric_limits<uint64_t>::max()));  // end_seqno
    encode_to(extras, uint64_t{});  // VB UUID
    encode_to(extras, htonll(std::numeric_limits<uint64_t>::min()));  // snap_start
    encode_to(extras, htonll(std::numeric_limits<uint64_t>::max()));  // snap_end

    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_DCP_STREAM_REQ, extras,
                     /*key*/{}, /*value*/{});

    return frame;
}


MutationInfo MemcachedBinprotConnection::mutate(const Document& doc,
                                                uint16_t vbucket,
                                                const Greenstack::mutation_type_t type) {
    protocol_binary_command cmd;
    switch (type) {
    case Greenstack::MutationType::Add:
        cmd = PROTOCOL_BINARY_CMD_ADD;
        break;
    case Greenstack::MutationType::Set:
        cmd = PROTOCOL_BINARY_CMD_SET;
        break;
    case Greenstack::MutationType::Replace:
        cmd = PROTOCOL_BINARY_CMD_REPLACE;
        break;
    case Greenstack::MutationType::Append:
        cmd = PROTOCOL_BINARY_CMD_APPEND;
        break;
    case Greenstack::MutationType::Prepend:
        cmd = PROTOCOL_BINARY_CMD_PREPEND;
        break;

    default:
        throw std::runtime_error(
            "Not implemented for MBCP: " + std::to_string(type));
    }

    Frame frame;
    // @todo fix expirations
    mcbp_storage_command(frame, cmd, doc.info.id, doc.value, doc.info.flags, 0);

    auto* req = reinterpret_cast<protocol_binary_request_set*>(frame.payload.data());
    if (doc.info.compression != Greenstack::Compression::None) {
        if (doc.info.compression != Greenstack::Compression::Snappy) {
            throw BinprotConnectionError("Invalid compression for MCBP",
                                         PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED);
        }
        req->message.header.request.datatype = PROTOCOL_BINARY_DATATYPE_COMPRESSED;
    }

    if (doc.info.datatype != Greenstack::Datatype::Raw) {
        req->message.header.request.datatype |= PROTOCOL_BINARY_DATATYPE_JSON;
    }

    req->message.header.request.cas = doc.info.cas;
    sendFrame(frame);

    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_set*>(frame.payload.data());
    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Failed to store " + doc.info.id,
                                     rsp->message.header.response.status);
    }

    MutationInfo info;
    info.cas = rsp->message.header.response.cas;
    // @todo add the rest of the fields
    return info;
}

void MemcachedBinprotConnection::setDatatypeSupport(bool enable) {
    std::array<bool, 4> requested;
    std::copy(features.begin(), features.end(), requested.begin());
    requested[0] = enable;
    setFeatures("mcbp", requested);

    if (enable && !features[0]) {
        throw std::runtime_error("Failed to enable datatype");
    }
}

void MemcachedBinprotConnection::setMutationSeqnoSupport(bool enable) {
    std::array<bool, 4> requested;
    std::copy(features.begin(), features.end(), requested.begin());
    requested[2] = enable;
    setFeatures("mcbp", requested);

    if (enable && !features[2]) {
        throw std::runtime_error("Failed to enable datatype");
    }

}

void MemcachedBinprotConnection::setXattrSupport(bool enable) {
    std::array<bool, 4> requested;
    std::copy(features.begin(), features.end(), requested.begin());
    requested[3] = enable;
    setFeatures("mcbp", requested);

    if (enable && !features[3]) {
        throw std::runtime_error("Failed to enable datatype");
    }
}

unique_cJSON_ptr MemcachedBinprotConnection::stats(const std::string& subcommand) {
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_STAT, std::vector<uint8_t>(),
                     subcommand, std::vector<uint8_t>());
    sendFrame(frame);
    unique_cJSON_ptr ret(cJSON_CreateObject());

    int counter = 0;

    while (true) {
        recvFrame(frame);
        auto* bytes = frame.payload.data();
        auto* rsp = reinterpret_cast<protocol_binary_response_stats*>(bytes);
        auto& header = rsp->message.header.response;
        if (header.status != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            throw BinprotConnectionError("Stats failed",
                                         header.status);
        }

        if (header.bodylen == 0) {
            // The stats EOF packet
            break;
        } else {
            std::string key((const char*)(rsp + 1), header.keylen);
            if (key.empty()) {
                key = std::to_string(counter++);
            }
            std::string value((const char*)(rsp + 1) + header.keylen,
                              header.bodylen - header.keylen);

            if (value == "false") {
                cJSON_AddFalseToObject(ret.get(), key.c_str());
            } else if (value == "true") {
                cJSON_AddTrueToObject(ret.get(), key.c_str());
            } else {
                try {
                    int64_t val = std::stoll(value);
                    cJSON_AddNumberToObject(ret.get(), key.c_str(), val);
                } catch (...) {
                    cJSON_AddStringToObject(ret.get(), key.c_str(),
                                            value.c_str());
                }
            }
        }
    }

    return ret;
}

void MemcachedBinprotConnection::configureEwouldBlockEngine(
    const EWBEngineMode& mode, ENGINE_ERROR_CODE err_code, uint32_t value,
    const std::string& key) {

    request_ewouldblock_ctl request;
    memset(request.bytes, 0, sizeof(request.bytes));
    request.message.header.request.magic = 0x80;
    request.message.header.request.opcode = PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL;
    request.message.header.request.extlen = 12;
    request.message.header.request.keylen = ntohs((short)key.size());
    request.message.header.request.bodylen = htonl(12 + key.size());
    request.message.body.inject_error = htonl(err_code);
    request.message.body.mode = htonl(static_cast<uint32_t>(mode));
    request.message.body.value = htonl(value);

    Frame frame;
    frame.payload.resize(sizeof(request.bytes) + key.size());
    memcpy(frame.payload.data(), request.bytes, sizeof(request.bytes));
    memcpy(frame.payload.data() + sizeof(request.bytes), key.data(),
           key.size());
    sendFrame(frame);

    recvFrame(frame);
    auto* bytes = frame.payload.data();
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(bytes);
    auto& header = rsp->message.header.response;
    if (header.status != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Failed to configure ewouldblock engine",
                                     header.status);
    }
}

void MemcachedBinprotConnection::reloadAuditConfiguration() {
    Frame frame;
    mcbp_raw_command(frame,
                     PROTOCOL_BINARY_CMD_AUDIT_CONFIG_RELOAD,
                     std::vector<uint8_t>(), "",
                     std::vector<uint8_t>());

    sendFrame(frame);
    recvFrame(frame);

    auto* bytes = frame.payload.data();
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(bytes);
    auto& header = rsp->message.header.response;
    if (header.status != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Failed to reload audit configuration",
                                     header.status);
    }
}

void MemcachedBinprotConnection::hello(const std::string& userAgent,
                                       const std::string& userAgentVersion,
                                       const std::string& comment) {
    std::array<bool, 4> requested;
    std::copy(features.begin(), features.end(), requested.begin());
    setFeatures(userAgent + " " + userAgentVersion, requested);

    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_SASL_LIST_MECHS, {}, {}, {});
    sendFrame(frame);
    recvFrame(frame);
    auto *rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());
    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Failed to fetch sasl mechanisms",
                                     rsp->message.header.response.status);
    }

    saslMechanisms.resize(rsp->message.header.response.bodylen);
    saslMechanisms.assign((const char*)(rsp + 1),
                          rsp->message.header.response.bodylen);
}

void MemcachedBinprotConnection::setFeatures(const std::string& agent,
                                             const std::array<bool, 4>& requested) {

    std::vector<uint16_t> feat;
    if (requested[0]) {
        feat.push_back(htons(uint16_t(mcbp::Feature::DATATYPE)));
    }

    if (requested[1]) {
        feat.push_back(htons(uint16_t(mcbp::Feature::TCPNODELAY)));
    }

    if (requested[2]) {
        feat.push_back(htons(uint16_t(mcbp::Feature::MUTATION_SEQNO)));
    }

    if (requested[3]) {
        feat.push_back(htons(uint16_t(mcbp::Feature::XATTR)));
    }

    std::vector<uint8_t> data(feat.size() * sizeof(feat.at(0)));
    memcpy(data.data(), feat.data(), data.size());

    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_HELLO, {}, agent, data);

    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());
    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("Failed to say hello",
                                     rsp->message.header.response.status);
    }

    // Validate the result!
    if ((rsp->message.header.response.bodylen & 1) != 0) {
        throw BinprotConnectionError("Invalid response returned",
                                     PROTOCOL_BINARY_RESPONSE_EINVAL);
    }

    std::vector<uint16_t> enabled;
    enabled.resize(rsp->message.header.response.bodylen / 2);
    memcpy(enabled.data(), (rsp + 1), rsp->message.header.response.bodylen);
    for (auto val : enabled) {
        val = ntohs(val);
        switch (mcbp::Feature(val)) {
        case mcbp::Feature::DATATYPE:
            features[0] = true;
            break;
        case mcbp::Feature::TCPNODELAY:
            features[1] = true;
            break;
        case mcbp::Feature::MUTATION_SEQNO:
            features[2] = true;
            break;
        case mcbp::Feature::XATTR:
            features[3] = true;
            break;
        default:
            throw std::runtime_error("Unsupported feature returned");
        }
    }
}

std::string MemcachedBinprotConnection::ioctl_get(const std::string& key) {
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_IOCTL_GET, {}, key, {});

    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());
    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("ioctl_get \"" + key + "\" failed.",
                                     rsp->message.header.response.status);
    }

    return std::string{(const char*)(rsp + 1),
                       rsp->message.header.response.bodylen};
}

void MemcachedBinprotConnection::ioctl_set(const std::string& key,
                                           const std::string& value) {
    Frame frame;
    std::vector<uint8_t> val(value.size());
    memcpy(val.data(), value.data(), value.size());
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_IOCTL_SET, {}, key, val);

    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(frame.payload.data());
    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        throw BinprotConnectionError("ioctl_set \"" + key + "\" failed.",
                                     rsp->message.header.response.status);
    }
}

uint64_t MemcachedBinprotConnection::increment(const std::string& key,
                                               uint64_t delta,
                                               uint64_t initial,
                                               rel_time_t exptime,
                                               MutationInfo* info) {
    return incr_decr(PROTOCOL_BINARY_CMD_INCREMENT, key, delta, initial,
                     exptime, info);
}

uint64_t MemcachedBinprotConnection::decrement(const std::string& key,
                                               uint64_t delta,
                                               uint64_t initial,
                                               rel_time_t exptime,
                                               MutationInfo* info) {
    return incr_decr(PROTOCOL_BINARY_CMD_DECREMENT, key, delta, initial,
                     exptime, info);
}

uint64_t MemcachedBinprotConnection::incr_decr(protocol_binary_command opcode,
                                               const std::string& key,
                                               uint64_t delta, uint64_t initial,
                                               rel_time_t exptime,
                                               MutationInfo* info) {

    // Data should be sent in network byte order
    delta = htonll(delta);
    initial = htonl(initial);
    exptime = htonl(exptime);

    std::vector<uint8_t> ext(sizeof(delta) + sizeof(initial) + sizeof(exptime));
    memcpy(ext.data(), &delta, sizeof(delta));
    memcpy(ext.data() + sizeof(delta), &initial, sizeof(initial));
    memcpy(ext.data() + sizeof(delta) + sizeof(initial), &exptime,
           sizeof(exptime));

    Frame frame;
    mcbp_raw_command(frame, opcode, ext, key, {});

    sendFrame(frame);
    recvFrame(frame);
    auto* rsp = reinterpret_cast<protocol_binary_response_incr*>(frame.payload.data());
    if (rsp->message.header.response.status !=
        PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        if (opcode == PROTOCOL_BINARY_CMD_INCREMENT) {
            throw BinprotConnectionError("incr \"" + key + "\" failed.",
                                         rsp->message.header.response.status);
        } else {
            throw BinprotConnectionError("decr \"" + key + "\" failed.",
                                         rsp->message.header.response.status);
        }
    }

    if (info != nullptr) {
        // Reset all values to 0xff to indicate that they are not set
        memset(info, 0xff, sizeof(*info));
        info->cas = rsp->message.header.response.cas;
    }

    uint8_t* payload = rsp->bytes + sizeof(rsp->bytes);
    if (rsp->message.header.response.extlen == 16) {
        // It contains uuid and seqno
        if (info != nullptr) {
            uint64_t* dptr = reinterpret_cast<uint64_t*>(payload);
            info->vbucketuuid = ntohll(dptr[0]);
            info->seqno = ntohll(dptr[1]);
        }
        payload += 16;
    } else if (rsp->message.header.response.extlen != 0) {
        throw std::runtime_error("Unknown extsize return from incr/decr");
    }

    uint64_t ret;
    memcpy(&ret, payload, sizeof(ret));
    return ntohll(ret);
}
