// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026  OtterStax

#include "scheduler/scheduler.hpp"

#include "otterbrix/parser/parser.hpp"
#include "utility/logger.hpp"
#include "utility/tracy_profiler.hpp"

#include <actor-zeta.hpp>

#include <cassert>
#include <chrono>
#include <list>
#include <memory_resource>
#include <utility>

Scheduler::Scheduler(std::pmr::memory_resource* res,
                     std::size_t worker_count,
                     parser_factory_fn parser_factory,
                     const otterstax::names::alias_registry_t& aliases,
                     actor_zeta::address_t sql_connection_manager,
                     actor_zeta::address_t pg_connection_manager,
                     actor_zeta::address_t ch_connection_manager,
                     actor_zeta::address_t otterbrix_manager,
                     actor_zeta::address_t catalog_manager,
                     actor_zeta::address_t s3_manager,
                     actor_zeta::address_t file_manager,
                     actor_zeta::address_t kafka_manager)
    : resource_(res)
    , log_(get_logger(logger_tag::SCHEDULER))
    , workers_(res)
    , worker_addresses_(res) {
    assert(log_.is_valid());
    assert(res != nullptr);
    assert(worker_count > 0);
    assert(parser_factory != nullptr);

    // Each Worker owns its own parser instance — no shared objects between actors
    // (codex rule 10). The factory is a plain function pointer (rule 14: no
    // std::function); the built parser is moved into the Worker. s3/file manager
    // addresses are forwarded so each Worker can dispatch external-table
    // statements (CREATE EXTERNAL TABLE / COPY ... TO) without touching shared
    // state.
    workers_.reserve(worker_count);
    worker_addresses_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        auto worker = actor_zeta::spawn<Worker>(res,
                                                i,
                                                worker_count,
                                                parser_factory(res, aliases),
                                                sql_connection_manager,
                                                pg_connection_manager,
                                                ch_connection_manager,
                                                otterbrix_manager,
                                                catalog_manager,
                                                s3_manager,
                                                file_manager,
                                                kafka_manager);
        worker_addresses_.push_back(worker->address());
        workers_.push_back(std::move(worker));
    }

    log_->info("Scheduler initialized with {} workers", worker_count);
}

Scheduler::~Scheduler() = default;

std::pair<bool, actor_zeta::detail::enqueue_result>
Scheduler::enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
    return mailbox_.enqueue(std::move(msg));
}

actor_zeta::behavior_t Scheduler::behavior(actor_zeta::mailbox::message* msg) {
    auto cmd = msg->command();
    if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::execute>) {
        co_await actor_zeta::dispatch(this, &Scheduler::execute, msg);
    } else if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::execute_statement>) {
        co_await actor_zeta::dispatch(this, &Scheduler::execute_statement, msg);
    } else if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::execute_prepared_statement>) {
        co_await actor_zeta::dispatch(this, &Scheduler::execute_prepared_statement, msg);
    } else if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::prepare_schema>) {
        co_await actor_zeta::dispatch(this, &Scheduler::prepare_schema, msg);
    } else if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::close_session>) {
        co_await actor_zeta::dispatch(this, &Scheduler::close_session, msg);
    } else if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::close_statement>) {
        co_await actor_zeta::dispatch(this, &Scheduler::close_statement, msg);
    } else if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::execute_plan>) {
        co_await actor_zeta::dispatch(this, &Scheduler::execute_plan, msg);
    } else if (cmd == actor_zeta::msg_id<Scheduler, &Scheduler::prepare_plan>) {
        co_await actor_zeta::dispatch(this, &Scheduler::prepare_plan, msg);
    }
}

// ─── Handlers: route by session hash to a Worker, forward the Worker's future ──
// Tracy zones are per-thread and strictly nested; the loop thread interleaves
// these coroutines, so a zone spanning the co_await would close out of order.
// Each handler therefore zones only its synchronous part — the routing inside
// route() — and awaits outside of it.

template<typename Method, typename... Args>
actor_zeta::unique_future<Scheduler::session_result>
Scheduler::route(Method method, session_hash_t id, Args... args) {
    // Coroutine parameters own their values while queued behind another request.
    auto [lane, first] = request_lanes_.try_emplace(id);
    if (!first) {
        lane->second.emplace_back(resource());
        auto turn = lane->second.back().get_future();
        co_await std::move(turn);
    }
    const auto idx = id % workers_.size();
    auto [needs_sched, fut] = actor_zeta::send(worker_addresses_[idx], method, id, std::move(args)...);
    auto result = co_await std::move(fut);
    auto& waiting = request_lanes_.at(id);
    if (waiting.empty()) request_lanes_.erase(id);
    else {
        waiting.front().set_value(true);
        waiting.pop_front();
    }
    co_return result;
}

actor_zeta::unique_future<Scheduler::session_result> Scheduler::execute(session_hash_t id, std::string sql, execution_context context) {
    if (!start_request(context)) {
        co_return core::error_t{core::error_code_t::invalid_parameter,
                               std::pmr::string{"connection session is closing or already executing a request", resource()}};
    }

    unique_future<session_result> fut;
    {
        OTX_ZONE_N("Scheduler::execute");
        fut = route(&Worker::execute, id, std::move(sql), context);
    }
    auto result = co_await std::move(fut);
    finish_request(context);
    co_return result;
}

actor_zeta::unique_future<Scheduler::session_result> Scheduler::execute_statement(session_hash_t id, execution_context context) {
    if (!start_request(context)) {
        co_return core::error_t{core::error_code_t::invalid_parameter,
                               std::pmr::string{"connection session is closing or already executing a request", resource()}};
    }

    unique_future<session_result> fut;
    {
        OTX_ZONE_N("Scheduler::execute_statement");
        fut = route(&Worker::execute_statement, id, context);
    }
    auto result = co_await std::move(fut);
    finish_request(context);
    co_return result;
}

actor_zeta::unique_future<Scheduler::session_result>
Scheduler::execute_prepared_statement(session_hash_t id,
                                      std::pmr::vector<components::types::logical_value_t> parameters, execution_context context) {
    if (!start_request(context)) {
        co_return core::error_t{core::error_code_t::invalid_parameter,
                               std::pmr::string{"connection session is closing or already executing a request", resource()}};
    }

    unique_future<session_result> fut;
    {
        OTX_ZONE_N("Scheduler::execute_prepared_statement");
        fut = route(&Worker::execute_prepared_statement, id, std::move(parameters), context);
    }
    auto result = co_await std::move(fut);
    finish_request(context);
    co_return result;
}

actor_zeta::unique_future<Scheduler::session_result> Scheduler::prepare_schema(session_hash_t id, std::string sql, execution_context context) {
    if (!start_request(context)) {
        co_return core::error_t{core::error_code_t::invalid_parameter,
                               std::pmr::string{"connection session is closing or already executing a request", resource()}};
    }

    unique_future<session_result> fut;
    {
        OTX_ZONE_N("Scheduler::prepare_schema");
        fut = route(&Worker::prepare_schema, id, std::move(sql), context);
    }
    auto result = co_await std::move(fut);
    finish_request(context);
    co_return result;
}

actor_zeta::unique_future<Scheduler::session_result> Scheduler::close_statement(session_hash_t id) {
    unique_future<session_result> fut;
    {
        OTX_ZONE_N("Scheduler::close_statement");
        fut = route(&Worker::close_statement, id);
    }
    co_return co_await std::move(fut);
}

actor_zeta::unique_future<Scheduler::session_result> Scheduler::execute_plan(session_hash_t id,
                                                                             ParsedQueryDataPtr data) {
    unique_future<session_result> fut;
    {
        OTX_ZONE_N("Scheduler::execute_plan");
        fut = route(&Worker::execute_plan, id, std::move(data));
    }
    co_return co_await std::move(fut);
}

actor_zeta::unique_future<Scheduler::session_result> Scheduler::prepare_plan(session_hash_t id,
                                                                             ParsedQueryDataPtr data) {
    unique_future<session_result> fut;
    {
        OTX_ZONE_N("Scheduler::prepare_plan");
        fut = route(&Worker::prepare_plan, id, std::move(data));
    }
    co_return co_await std::move(fut);
}

bool Scheduler::start_request(const execution_context& context) {
    if (!context.persistent) return true;
    auto& work = connections_[context.engine_session];
    if (work.closing || work.pending != 0) return false;
    ++work.pending;
    return true;
}

void Scheduler::finish_request(const execution_context& context) {
    if (!context.persistent) return;
    auto it = connections_.find(context.engine_session);
    assert(it != connections_.end() && it->second.pending > 0);
    auto& work = it->second;
    if (--work.pending == 0 && work.drained) work.drained->set_value(true);
}

actor_zeta::unique_future<Scheduler::session_result> Scheduler::close_session(execution_context context) {
    const auto key = context.engine_session;
    auto& work = connections_[key];
    if (work.closing) {
        co_return core::error_t{core::error_code_t::invalid_parameter,
                               std::pmr::string{"connection cleanup already requested", resource()}};
    }
    work.closing = true;
    if (work.pending != 0) {
        work.drained.emplace(resource());
        auto drained = work.drained->get_future();
        co_await std::move(drained);
    }
    context.in_transaction = false;
    const auto request = next_request_id();
    auto result = co_await route(&Worker::execute, request, std::string{"ROLLBACK;"}, context);
    connections_.erase(key);
    co_return result;
}
