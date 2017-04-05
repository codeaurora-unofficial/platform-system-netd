/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "dns_benchmark"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <android-base/stringprintf.h>
#include <benchmark/benchmark.h>
#include <utils/String16.h>
#include <utils/StrongPointer.h>

#include "dns_responder_client.h"
#include "NetdClient.h"
#include "android/net/metrics/INetdEventListener.h"

using android::base::StringPrintf;
using android::net::metrics::INetdEventListener;

constexpr int MIN_THREADS = 1;
constexpr int MAX_THREADS = 32;

class DnsFixture : public ::benchmark::Fixture {
protected:
    static constexpr unsigned num_hosts = 1000;
    DnsResponderClient dns;
    std::vector<DnsResponderClient::Mapping> mappings;
    std::vector<std::unique_ptr<test::DNSResponder>> mDns;

public:
    void SetUp(const ::benchmark::State& state) override {
        if (state.thread_index == 0) {
            dns.SetUp();

            std::vector<std::string> domains = { "example.com" };
            std::vector<std::string> servers;
            dns.SetupMappings(num_hosts, domains, &mappings);

            dns.SetupDNSServers(MAXNS, mappings, &mDns, &servers);

            const std::vector<int> mDefaultParams_Binder = { 300, 25, 8, 8 };
            dns.SetResolversForNetwork(servers, domains, mDefaultParams_Binder);
        }
    }

    void TearDown(const ::benchmark::State& state) override {
        if (state.thread_index == 0) {
            dns.ShutdownDNSServers(&mDns);
            dns.TearDown();
        }
    }

    std::vector<DnsResponderClient::Mapping> const& getMappings() const {
        return mappings;
    }

    android::sp<android::net::INetd> getNetd() const {
        return dns.mNetdSrv;
    }

    void getaddrinfo_until_done(benchmark::State &state) {
        while (state.KeepRunning()) {
            const uint32_t ofs = arc4random_uniform(getMappings().size());
            const auto& mapping = getMappings()[ofs];
            addrinfo* result = nullptr;
            if (getaddrinfo(mapping.host.c_str(), nullptr, nullptr, &result)) {
                state.SkipWithError(StringPrintf("getaddrinfo failed with errno=%d",
                        errno).c_str());
                break;
            }
            if (result) {
                freeaddrinfo(result);
                result = nullptr;
            }
        }
    }

    void benchmark_at_reporting_level(benchmark::State &state, int metricsLevel) {
        const bool isMaster = (state.thread_index == 0);
        int oldMetricsLevel;

        // SETUP
        if (isMaster) {
            auto rv = getNetd()->getMetricsReportingLevel(&oldMetricsLevel);
            if (!rv.isOk()) {
                state.SkipWithError(StringPrintf("Failed saving metrics reporting level: %s",
                        rv.toString8().string()).c_str());
                return;
            }
            rv = getNetd()->setMetricsReportingLevel(metricsLevel);
            if (!rv.isOk()) {
                state.SkipWithError(StringPrintf("Failed changing metrics reporting: %s",
                        rv.toString8().string()).c_str());
                return;
            }
        }

        // TEST
        getaddrinfo_until_done(state);

        // TEARDOWN
        if (isMaster) {
            auto rv = getNetd()->setMetricsReportingLevel(oldMetricsLevel);
            if (!rv.isOk()) {
                state.SkipWithError(StringPrintf("Failed restoring metrics reporting level: %s",
                        rv.toString8().string()).c_str());
                return;
            }
        }
    }
};

// DNS calls without any metrics logged or sent.
BENCHMARK_DEFINE_F(DnsFixture, getaddrinfo_log_nothing)(benchmark::State& state) {
    benchmark_at_reporting_level(state, INetdEventListener::REPORTING_LEVEL_NONE);
}
BENCHMARK_REGISTER_F(DnsFixture, getaddrinfo_log_nothing)
    ->ThreadRange(MIN_THREADS, MAX_THREADS)
    ->UseRealTime();

// DNS calls with metrics only (netId, latency, return code) sent to the system server.
BENCHMARK_DEFINE_F(DnsFixture, getaddrinfo_log_metrics)(benchmark::State& state) {
    benchmark_at_reporting_level(state, INetdEventListener::REPORTING_LEVEL_METRICS);
}
BENCHMARK_REGISTER_F(DnsFixture, getaddrinfo_log_metrics)
    ->ThreadRange(MIN_THREADS, MAX_THREADS)
    ->UseRealTime();

// DNS calls with all information logged and sent to the system server.
BENCHMARK_DEFINE_F(DnsFixture, getaddrinfo_log_everything)(benchmark::State& state) {
    benchmark_at_reporting_level(state, INetdEventListener::REPORTING_LEVEL_FULL);
}
BENCHMARK_REGISTER_F(DnsFixture, getaddrinfo_log_everything)
    ->ThreadRange(MIN_THREADS, MAX_THREADS)
    ->UseRealTime();
