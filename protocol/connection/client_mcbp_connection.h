/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
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
#pragma once

#include <array>
#include "config.h"

#include "client_connection.h"

inline std::string formatMcbpExceptionMsg(const std::string& prefix,
                                          uint16_t reason) {
    // Format the error message
    std::string errormessage(prefix);
    errormessage.append(": ");
    auto err = static_cast<protocol_binary_response_status>(reason);
    errormessage.append(memcached_status_2_text(err));
    errormessage.append(" (");
    errormessage.append(std::to_string(reason));
    errormessage.append(")");
    return errormessage;
}

class BinprotConnectionError : public ConnectionError {
public:
public:
    explicit BinprotConnectionError(const char* prefix,
                                    uint16_t reason_)
        : ConnectionError(formatMcbpExceptionMsg(prefix, reason_).c_str()),
          reason(reason_) {
    }

    explicit BinprotConnectionError(const std::string& prefix,
                                    uint16_t reason_)
        : BinprotConnectionError(prefix.c_str(), reason_) {
    }

    uint16_t getReason() const override {
        return reason;
    }

    Protocol getProtocol() const override {
        return Protocol::Memcached;
    }

    bool isInvalidArguments() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    bool isAlreadyExists() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
    }

    bool isNotFound() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
    }

    bool isNotMyVbucket() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET;
    }

    bool isNotStored() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_NOT_STORED;
    }

    bool isAccessDenied() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_EACCESS;
    }

    bool isDeltaBadval() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL;
    }

    bool isAuthError() const override {
        return reason == PROTOCOL_BINARY_RESPONSE_AUTH_ERROR;
    }

private:
    uint16_t reason;
};

class MemcachedBinprotConnection : public MemcachedConnection {
public:
    MemcachedBinprotConnection(const std::string& host, in_port_t port,
                               sa_family_t family, bool ssl)
        : MemcachedConnection(host, port, family, ssl, Protocol::Memcached) {
        std::fill(features.begin(), features.end(), false);
    }

    std::unique_ptr<MemcachedConnection> clone() override;

    std::string to_string() override;

    void authenticate(const std::string& username,
                      const std::string& password,
                      const std::string& mech) override;

    void createBucket(const std::string& name,
                      const std::string& config,
                      const Greenstack::BucketType& type) override;


    void deleteBucket(const std::string& name) override;

    void selectBucket(const std::string& name) override;

    std::vector<std::string> listBuckets() override;

    Document get(const std::string& id, uint16_t vbucket) override;

    Frame encodeCmdGet(const std::string& id, uint16_t vbucket) override;

    Frame encodeCmdDcpOpen() override;

    Frame encodeCmdDcpStreamReq() override;

    MutationInfo mutate(const Document& doc, uint16_t vbucket,
                        const Greenstack::mutation_type_t type) override;

    unique_cJSON_ptr stats(const std::string& subcommand) override;

    void reloadAuditConfiguration() override;

    void sendFrame(const Frame& frame) override;

    void recvFrame(Frame& frame) override;

    void hello(const std::string& userAgent,
               const std::string& userAgentVersion,
               const std::string& comment) override;

    void setDatatypeSupport(bool enable);

    void setMutationSeqnoSupport(bool enable);

    void setXattrSupport(bool enable);

    std::string ioctl_get(const std::string& key) override;

    void ioctl_set(const std::string& key,
                   const std::string& value) override;

    uint64_t increment(const std::string& key,
                       uint64_t delta,
                       uint64_t initial,
                       rel_time_t exptime,
                       MutationInfo* info) override;

    uint64_t decrement(const std::string& key,
                       uint64_t delta,
                       uint64_t initial,
                       rel_time_t exptime,
                       MutationInfo* info) override;

    void configureEwouldBlockEngine(const EWBEngineMode& mode,
                                    ENGINE_ERROR_CODE err_code,
                                    uint32_t value,
                                    const std::string& key) override;

    std::array<bool, 4> features;

protected:

    uint64_t incr_decr(protocol_binary_command opcode,
                       const std::string& key,
                       uint64_t delta,
                       uint64_t initial,
                       rel_time_t exptime,
                       MutationInfo* info);

    /**
     * Set the features on the server by using the MCBP hello command
     *
     * The internal `features` array is updated with the result sent back
     * from the server.
     *
     * @param agent the agent name provided by the client
     * @param feat the featureset to enable
     */
    void setFeatures(const std::string& agent,
                     const std::array<bool, 4>& requested);
};
