/**
 * @file live_bridge.cpp
 * @brief The Blender add-on's pipe (see live_bridge.hpp).
 *
 * Threads: the worker owns the pipe and waits on overlapped I/O or its stop event. A request it cannot answer itself
 * goes through s_exchange to the frame tick on the main thread, the only thread that runs Lua or touches the console.
 */

#include "live_bridge.hpp"

#include "console_vars.hpp"
#include "constants.hpp"
#include "core/bridge_protocol.hpp"
#include "game_memory.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>
#include <sddl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        /// The Lua chunk name, which the engine's error handler prints in kcd.log for a failed run.
        constexpr const char *CHUNK_NAME = "=AnimationFramework bridge";
        /// The longest part of a request or an answer that one log line quotes.
        constexpr std::size_t LOG_QUOTE_LENGTH = 160;
        /**
         * @brief The pipe's access list around the SID of the user the game runs as.
         * @details SYSTEM and that user get full access. The default access list of a pipe that an elevated game
         *          creates gives write access to Administrators only, a group that a Blender without elevation holds
         *          as deny-only. The explicit medium label keeps the pipe writable from medium integrity.
         */
        constexpr std::wstring_view PIPE_SDDL_HEAD = L"D:P(A;;GA;;;SY)(A;;GA;;;";
        constexpr std::wstring_view PIPE_SDDL_TAIL = L")S:(ML;;NW;;;ME)";

        struct HandleCloser
        {
            void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
        };
        using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

        /** @brief Owns @p handle. INVALID_HANDLE_VALUE, which CreateNamedPipe returns on failure, owns nothing. */
        [[nodiscard]] UniqueHandle own(HANDLE handle) noexcept
        {
            return UniqueHandle{handle == INVALID_HANDLE_VALUE ? nullptr : handle};
        }

        struct LocalFreer
        {
            void operator()(void *block) const noexcept { LocalFree(block); }
        };
        using LocalBlock = std::unique_ptr<void, LocalFreer>;

        /**
         * @brief The one request between the worker and the main thread.
         * @details The worker posts a request and waits for its answer. The frame tick takes the request, runs it
         *          without the lock and posts the answer. The worker withdraws a request that no frame took in time. An
         *          empty answer means that the run itself failed.
         */
        struct Exchange
        {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::optional<bridge::Request> request;
            bool running{false};
            std::optional<std::string> answer;
        };
        Exchange s_exchange;
        /// Set while a request waits, so a frame without one costs a single load.
        std::atomic<bool> s_request_waiting{false};

        // Set before the worker starts, which orders them before any request the frame tick sees. Cleared after the
        // worker joined and the hooks retired.
        std::uintptr_t s_genv = 0;
        std::uintptr_t s_script_system_vtable = 0;
        std::optional<DMK::StoppableWorker> s_worker;

        /** @brief @p text for one log line: tabs and line breaks as spaces, cut at LOG_QUOTE_LENGTH. */
        [[nodiscard]] std::string printable(std::string_view text)
        {
            std::string out{text.substr(0, LOG_QUOTE_LENGTH)};
            for (char &c : out)
            {
                c = c == '\t' || c == '\n' || c == '\r' ? ' ' : c;
            }
            return out;
        }

        /** @brief The game's script system when its vtable is the one resolved at init, else nullptr. */
        [[nodiscard]] void *script_system() noexcept
        {
            if (s_genv == 0 || s_script_system_vtable == 0)
            {
                return nullptr;
            }
            const std::uintptr_t object = read_ptr(s_genv + constants::GENV_SCRIPT_SYSTEM_OFFSET);
            return object != 0 && read_ptr(object) == s_script_system_vtable ? reinterpret_cast<void *>(object)
                                                                             : nullptr;
        }

        /**
         * @brief Runs @p request's Lua and builds its answer. Main thread.
         * @throws std::bad_alloc If the text cannot be built.
         */
        [[nodiscard]] std::string execute(const bridge::Request &request)
        {
            void *scripts = script_system();
            if (scripts == nullptr)
            {
                return bridge::answer(false, "the game's script system is not up");
            }
            bool registered = false;
            void *error_cvar = find_or_register_string_cvar(bridge::ERROR_CVAR, bridge::ERROR_CVAR_HELP, registered);
            if (error_cvar == nullptr)
            {
                return bridge::answer(false, "the game's console is not up");
            }
            set_cvar_text(error_cvar, std::string{});
            const std::string code = bridge::lua_for(request);
            const bool ran = vcall<bool>(
                scripts,
                constants::SCRIPT_EXECUTE_BUFFER_SLOT,
                code.c_str(),
                code.size(),
                CHUNK_NAME,
                static_cast<void *>(nullptr)
            );
            // The chunk's pcall reports a run error through the variable and still returns normally. A false return
            // with an empty variable is a load error, or an error inside the hand-off itself.
            const std::string error{cvar_text(error_cvar)};
            if (!error.empty())
            {
                return bridge::answer(false, error);
            }
            if (!ran)
            {
                return bridge::answer(false, "the game could not run the request (kcd.log has the Lua error)");
            }
            return bridge::answer(true, "done");
        }

        /**
         * @brief Hands @p request to the frame tick and waits for its answer, or withdraws it. Worker thread.
         * @return The answer, or nothing when the worker stops first.
         * @throws std::bad_alloc If the answer cannot be built.
         */
        [[nodiscard]] std::optional<std::string>
        run_on_main_thread(bridge::Request request, const std::stop_token &stop)
        {
            std::unique_lock lock{s_exchange.mutex};
            s_exchange.answer.reset();
            s_exchange.request = std::move(request);
            s_request_waiting.store(true, std::memory_order_release);
            const auto answered = []() noexcept { return s_exchange.answer.has_value(); };
            bool done = s_exchange.changed.wait_for(lock, stop, constants::BRIDGE_FRAME_DEADLINE, answered);
            if (!done && s_exchange.running)
            {
                // A frame took the request in time. Its run ends on its own, and a teardown retires the hooks before it
                // stops this worker, so no run stays half done.
                done = s_exchange.changed.wait(lock, stop, answered);
            }
            if (done)
            {
                std::string out = std::move(*s_exchange.answer);
                s_exchange.answer.reset();
                return out.empty() ? bridge::answer(false, "the request failed in the game") : out;
            }
            s_exchange.request.reset();
            s_request_waiting.store(false, std::memory_order_release);
            if (stop.stop_requested())
            {
                return std::nullopt;
            }
            return bridge::answer(
                false,
                std::format(
                    "the game ran no frame within {} s",
                    std::chrono::duration_cast<std::chrono::seconds>(constants::BRIDGE_FRAME_DEADLINE).count()
                )
            );
        }

        /**
         * @brief The answer to one request message. Worker thread.
         * @return The answer, or nothing when the worker stops first.
         * @throws std::bad_alloc If a text cannot be built.
         */
        [[nodiscard]] std::optional<std::string> handle_request(std::string_view message, const std::stop_token &stop)
        {
            auto request = bridge::parse_request(message);
            if (request && request->operation == bridge::Operation::Hello)
            {
                DMK::log().debug("Bridge: hello from the Blender add-on");
                return bridge::answer(true, std::format("{} {}", bridge::PROTOCOL_VERSION, version::VERSION_TAG));
            }
            std::optional<std::string> reply =
                request ? run_on_main_thread(std::move(*request), stop) : bridge::answer(false, request.error());
            if (reply)
            {
                DMK::log().info("Bridge: {} -> {}", printable(message), printable(*reply));
            }
            return reply;
        }

        /**
         * @brief The pipe's security descriptor (PIPE_SDDL_HEAD), or the Win32 error that stopped it.
         * @throws std::bad_alloc If the token buffer or the text cannot be allocated.
         */
        [[nodiscard]] std::expected<LocalBlock, DWORD> pipe_security()
        {
            HANDLE raw_token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            {
                return std::unexpected(GetLastError());
            }
            const UniqueHandle token{raw_token};
            // The size query fails by design (ERROR_INSUFFICIENT_BUFFER) and only reports the size.
            DWORD size = 0;
            GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
            if (size == 0)
            {
                return std::unexpected(GetLastError());
            }
            std::vector<std::byte> buffer(size);
            if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
            {
                return std::unexpected(GetLastError());
            }
            LPWSTR raw_sid = nullptr;
            if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(buffer.data())->User.Sid, &raw_sid))
            {
                return std::unexpected(GetLastError());
            }
            const LocalBlock sid{raw_sid};
            std::wstring sddl{PIPE_SDDL_HEAD};
            sddl += raw_sid;
            sddl += PIPE_SDDL_TAIL;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(),
                    SDDL_REVISION_1,
                    &descriptor,
                    nullptr
                ))
            {
                return std::unexpected(GetLastError());
            }
            return LocalBlock{descriptor};
        }

        enum class IoStatus : std::uint8_t
        {
            Done,
            /// A message longer than the buffer: the rest waits to be read.
            MoreData,
            /// The client closed its end.
            Closed,
            Stopped,
            Failed,
        };

        struct IoResult
        {
            IoStatus status{IoStatus::Failed};
            DWORD bytes{0};
            DWORD error{ERROR_SUCCESS};
        };

        [[nodiscard]] IoResult classify(DWORD error, DWORD bytes) noexcept
        {
            switch (error)
            {
            case ERROR_SUCCESS:
                return {IoStatus::Done, bytes, error};
            case ERROR_MORE_DATA:
                return {IoStatus::MoreData, bytes, error};
            case ERROR_BROKEN_PIPE:
            case ERROR_PIPE_NOT_CONNECTED:
            case ERROR_NO_DATA:
                return {IoStatus::Closed, bytes, error};
            case ERROR_OPERATION_ABORTED:
                return {IoStatus::Stopped, bytes, error};
            default:
                return {IoStatus::Failed, bytes, error};
            }
        }

        /**
         * @brief Finishes an overlapped operation on @p pipe that started with @p start_error, or cancels it when
         *        @p stop_event is set first.
         * @param start_error ERROR_SUCCESS when the operation completed at once, else the error its start reported.
         */
        [[nodiscard]] IoResult
        finish_io(HANDLE pipe, OVERLAPPED &overlapped, DWORD start_error, HANDLE stop_event) noexcept
        {
            if (start_error != ERROR_SUCCESS && start_error != ERROR_IO_PENDING)
            {
                return classify(start_error, 0);
            }
            if (start_error == ERROR_IO_PENDING)
            {
                const std::array<HANDLE, 2> events{
                    overlapped.hEvent,
                    stop_event,
                };
                if (WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE) !=
                    WAIT_OBJECT_0)
                {
                    // The kernel still owns the OVERLAPPED until the cancelled operation completes, so wait for it
                    // before the frame that holds it unwinds.
                    CancelIoEx(pipe, &overlapped);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                    return {IoStatus::Stopped, 0, ERROR_OPERATION_ABORTED};
                }
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(pipe, &overlapped, &bytes, FALSE))
            {
                return classify(GetLastError(), bytes);
            }
            return {IoStatus::Done, bytes, ERROR_SUCCESS};
        }

        [[nodiscard]] IoResult read_message(HANDLE pipe, HANDLE io_event, HANDLE stop_event, std::span<char> buffer)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = io_event;
            const BOOL done = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped);
            return finish_io(pipe, overlapped, done ? ERROR_SUCCESS : GetLastError(), stop_event);
        }

        [[nodiscard]] IoResult write_message(HANDLE pipe, HANDLE io_event, HANDLE stop_event, std::string_view text)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = io_event;
            const BOOL done = WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), nullptr, &overlapped);
            return finish_io(pipe, overlapped, done ? ERROR_SUCCESS : GetLastError(), stop_event);
        }

        /** @brief Waits for a client to open the pipe. */
        [[nodiscard]] IoResult await_client(HANDLE pipe, HANDLE io_event, HANDLE stop_event) noexcept
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = io_event;
            const BOOL done = ConnectNamedPipe(pipe, &overlapped);
            const DWORD error = done ? ERROR_SUCCESS : GetLastError();
            // A client that opened the pipe between its creation and this call is already connected.
            if (error == ERROR_PIPE_CONNECTED)
            {
                return {IoStatus::Done, 0, ERROR_SUCCESS};
            }
            return finish_io(pipe, overlapped, error, stop_event);
        }

        /**
         * @brief Answers one client's requests until it closes its end or the worker stops.
         * @throws std::bad_alloc If an answer cannot be built.
         */
        void serve_client(HANDLE pipe, HANDLE io_event, HANDLE stop_event, const std::stop_token &stop)
        {
            std::array<char, bridge::MAX_REQUEST_BYTES> buffer{};
            while (!stop.stop_requested())
            {
                IoResult received = read_message(pipe, io_event, stop_event, buffer);
                std::optional<std::string> reply;
                if (received.status == IoStatus::MoreData)
                {
                    // Message mode keeps the rest of an oversized request for the next reads. Drain it, so the next
                    // read starts on the client's next request.
                    while (received.status == IoStatus::MoreData)
                    {
                        received = read_message(pipe, io_event, stop_event, buffer);
                    }
                    if (received.status != IoStatus::Done)
                    {
                        return;
                    }
                    reply =
                        bridge::answer(false, std::format("request longer than {} bytes", bridge::MAX_REQUEST_BYTES));
                }
                else if (received.status == IoStatus::Done)
                {
                    reply = handle_request(std::string_view{buffer.data(), received.bytes}, stop);
                }
                else
                {
                    if (received.status == IoStatus::Failed)
                    {
                        DMK::log().warning("Bridge: a read failed (error {})", received.error);
                    }
                    return;
                }
                if (!reply)
                {
                    return;
                }
                if (const IoResult sent = write_message(pipe, io_event, stop_event, *reply);
                    sent.status != IoStatus::Done)
                {
                    if (sent.status == IoStatus::Failed)
                    {
                        DMK::log().warning("Bridge: an answer could not be sent (error {})", sent.error);
                    }
                    return;
                }
            }
        }

        /**
         * @brief The worker: creates the pipe and serves one client at a time until it stops.
         * @throws std::bad_alloc If a text cannot be built.
         */
        void listen(const std::stop_token &stop)
        {
            const UniqueHandle stop_event = own(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!stop_event)
            {
                DMK::log().error(
                    "Bridge: no stop event (error {}); the Blender add-on cannot reach the game",
                    GetLastError()
                );
                return;
            }
            const UniqueHandle io_event = own(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!io_event)
            {
                DMK::log().error(
                    "Bridge: no I/O event (error {}); the Blender add-on cannot reach the game",
                    GetLastError()
                );
                return;
            }
            const std::stop_callback wake{stop, [event = stop_event.get()]() noexcept { SetEvent(event); }};
            const auto retry_wait = static_cast<DWORD>(constants::BRIDGE_RETRY_INTERVAL.count());

            const std::expected<LocalBlock, DWORD> security = pipe_security();
            if (!security)
            {
                DMK::log().warning(
                    "Bridge: the pipe keeps the default access list (error {}); a Blender without elevation cannot "
                    "reach a game that runs as administrator",
                    security.error()
                );
            }
            SECURITY_ATTRIBUTES attributes{
                .nLength = sizeof(SECURITY_ATTRIBUTES),
                .lpSecurityDescriptor = security ? security->get() : nullptr,
                .bInheritHandle = FALSE,
            };

            bool reported_taken = false;
            while (!stop.stop_requested())
            {
                const UniqueHandle pipe = own(CreateNamedPipeA(
                    constants::BRIDGE_PIPE_NAME,
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1,
                    constants::BRIDGE_PIPE_BUFFER_BYTES,
                    constants::BRIDGE_PIPE_BUFFER_BYTES,
                    0,
                    &attributes
                ));
                if (!pipe)
                {
                    // FILE_FLAG_FIRST_PIPE_INSTANCE refuses a name that another process still holds, such as a
                    // framework generation that has not closed its pipe yet.
                    const DWORD error = GetLastError();
                    if (!reported_taken)
                    {
                        DMK::log().warning(
                            "Bridge: {} is not free (error {}); next try in {} ms",
                            constants::BRIDGE_PIPE_NAME,
                            error,
                            retry_wait
                        );
                        reported_taken = true;
                    }
                    WaitForSingleObject(stop_event.get(), retry_wait);
                    continue;
                }
                reported_taken = false;
                DMK::log().info("Bridge: the Blender add-on reaches the game through {}", constants::BRIDGE_PIPE_NAME);
                while (!stop.stop_requested())
                {
                    const IoResult connected = await_client(pipe.get(), io_event.get(), stop_event.get());
                    if (connected.status == IoStatus::Done)
                    {
                        serve_client(pipe.get(), io_event.get(), stop_event.get(), stop);
                        DisconnectNamedPipe(pipe.get());
                        continue;
                    }
                    if (connected.status != IoStatus::Stopped)
                    {
                        DMK::log().warning("Bridge: the wait for a client failed (error {})", connected.error);
                        WaitForSingleObject(stop_event.get(), retry_wait);
                    }
                    break;
                }
            }
        }

        /** @brief The worker's body: an exception ends the listener with a log line instead of the process. */
        void serve(std::stop_token stop) noexcept
        {
            try
            {
                listen(stop);
            }
            catch (const std::exception &error)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error, "Bridge: the listener stopped: {}", error.what());
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Bridge: the listener stopped");
            }
        }
    } // namespace

    void start_live_bridge(std::uintptr_t genv, std::uintptr_t script_system_vtable) noexcept
    {
        if (genv == 0 || script_system_vtable == 0)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Warning,
                "Bridge: the game's script system was not found; the Blender add-on cannot reach the game"
            );
            return;
        }
        s_genv = genv;
        s_script_system_vtable = script_system_vtable;
        try
        {
            s_worker.emplace("AnimationFramework.bridge", &serve);
        }
        catch (const std::exception &error)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Error, "Bridge: the listener did not start: {}", error.what());
            s_worker.reset();
            s_genv = 0;
            s_script_system_vtable = 0;
        }
    }

    void run_live_bridge_frame_work() noexcept
    {
        if (!s_request_waiting.load(std::memory_order_acquire))
        {
            return;
        }
        std::optional<bridge::Request> request;
        {
            const std::lock_guard lock{s_exchange.mutex};
            request = std::exchange(s_exchange.request, std::nullopt);
            // The mutex orders this store against the worker's next post, which stores true under it.
            s_request_waiting.store(false, std::memory_order_relaxed);
            s_exchange.running = request.has_value();
        }
        if (!request)
        {
            return;
        }
        std::string result;
        try
        {
            result = execute(*request);
        }
        catch (...)
        {
            // An empty answer tells the worker that the run failed. The worker builds the text off this thread.
            result.clear();
        }
        {
            const std::lock_guard lock{s_exchange.mutex};
            s_exchange.running = false;
            s_exchange.answer = std::move(result);
        }
        s_exchange.changed.notify_all();
    }

    void stop_live_bridge() noexcept
    {
        if (s_worker.has_value())
        {
            s_worker->shutdown();
            s_worker.reset();
        }
        {
            const std::lock_guard lock{s_exchange.mutex};
            s_exchange.request.reset();
            s_exchange.answer.reset();
            s_exchange.running = false;
            s_request_waiting.store(false, std::memory_order_relaxed);
        }
        s_genv = 0;
        s_script_system_vtable = 0;
    }
} // namespace AnimationFramework
