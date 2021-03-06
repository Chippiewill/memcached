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
#include "config.h"

#include "programs/utilities.h"
#include "testapp_environment.h"

#include <cJSON_utils.h>
#include <fstream>
#include <platform/dirutils.h>
#include <platform/strerror.h>

McdEnvironment::McdEnvironment() {
    initialize_openssl();
}

McdEnvironment::~McdEnvironment() {
    shutdown_openssl();
}

void McdEnvironment::SetUp() {
    cwd = cb::io::getcwd();
    SetupAuditFile();
    SetupIsaslPw();
}

void McdEnvironment::SetupIsaslPw() {
    isasl_file_name = SOURCE_ROOT;
    isasl_file_name.append("/tests/testapp/cbsaslpw.json");
    std::replace(isasl_file_name.begin(), isasl_file_name.end(), '\\', '/');

    // Add the file to the exec environment
    snprintf(isasl_env_var, sizeof(isasl_env_var), "CBSASL_PWFILE=%s",
             isasl_file_name.c_str());
    putenv(isasl_env_var);
}

void McdEnvironment::SetupAuditFile() {
    try {
        audit_file_name = cwd + "/" + cb::io::mktemp("audit.cfg");
        audit_log_dir = cwd + "/" + cb::io::mktemp("audit.log");
        const std::string descriptor = cwd + "/auditd";
        EXPECT_TRUE(cb::io::rmrf(audit_log_dir));
        cb::io::mkdirp(audit_log_dir);

        // Generate the auditd config file.
        audit_config.reset(cJSON_CreateObject());
        cJSON_AddNumberToObject(audit_config.get(), "version", 1);
        cJSON_AddFalseToObject(audit_config.get(), "auditd_enabled");
        cJSON_AddNumberToObject(audit_config.get(), "rotate_interval", 1440);
        cJSON_AddNumberToObject(audit_config.get(), "rotate_size", 20971520);
        cJSON_AddFalseToObject(audit_config.get(), "buffered");
        cJSON_AddStringToObject(audit_config.get(), "log_path",
                                audit_log_dir.c_str());
        cJSON_AddStringToObject(audit_config.get(), "descriptors_path",
                                descriptor.c_str());
        cJSON_AddItemToObject(audit_config.get(), "sync", cJSON_CreateArray());
        cJSON_AddItemToObject(audit_config.get(), "disabled",
                              cJSON_CreateArray());
    } catch (std::exception& e) {
        FAIL() << "Failed to generate audit configuration: " << e.what();
    }

    rewriteAuditConfig();
}

void McdEnvironment::TearDown() {
    // Cleanup Audit config file
    if (!audit_file_name.empty()) {
        EXPECT_TRUE(cb::io::rmrf(audit_file_name));
    }

    // Cleanup Audit log directory
    if (!audit_log_dir.empty()) {
        EXPECT_TRUE(cb::io::rmrf(audit_log_dir)) << cb_strerror();
    }
}

void McdEnvironment::rewriteAuditConfig() {
    try {
        std::string audit_text = to_string(audit_config);
        std::ofstream out(audit_file_name);
        out.write(audit_text.c_str(), audit_text.size());
        out.close();
    } catch (std::exception& e) {
        FAIL() << "Failed to store audit configuration: " << e.what();
    }
}

char McdEnvironment::isasl_env_var[256];
