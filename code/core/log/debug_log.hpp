#pragma once

#include "pch.hpp"

#if defined(__APP_FORCE_LOGS) || (defined(_DEBUG))

namespace Detail {
class AsyncSpamLogger {
	private:

		std::queue<std::string>     m_queue;
		std::mutex                  m_mutex;
		std::condition_variable_any m_cv; // Corrigido para _any para suportar o stop_token no wait
		std::jthread                m_worker;
		std::string                 m_last_line;

		AsyncSpamLogger() {
			// O jthread passa automaticamente o seu stop_token para a lambda
			m_worker = std::jthread([this](std::stop_token stop_token) {
				this->process_queue(stop_token);
			});
		}

		~AsyncSpamLogger() = default;

		void process_queue(std::stop_token stop_token) {
			while (!stop_token.stop_requested()) {
				std::string current_line;
				{
					std::unique_lock<std::mutex> lock(m_mutex);

					// CORREÇÃO: std::condition_variable_any::wait aceita 3 argumentos:
					// 1. O Lock, 2. O Stop Token, 3. O Predicado
					// Ele acorda se a fila tiver dados OU se a thread receber um pedido de parada.
					bool acquired = m_cv.wait(lock, stop_token, [this] {
						return !m_queue.empty();
					});

					// Se acordou mas a fila continua vazia, significa que fomos interrompidos para
					// fechar o app
					if (!acquired || m_queue.empty()) {
						continue;
					}

					current_line = std::move(m_queue.front());
					m_queue.pop();
				}

				if (current_line != m_last_line) {
					std::println("{}", current_line);
					m_last_line = std::move(current_line);
				}
			}
		}

	public:

		static AsyncSpamLogger& get_instance() {
			static AsyncSpamLogger instance;
			return instance;
		}

		AsyncSpamLogger(AsyncSpamLogger const&)            = delete;
		AsyncSpamLogger& operator=(AsyncSpamLogger const&) = delete;

		void push_log(std::string log_msg) {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_queue.push(std::move(log_msg));
			}
			m_cv.notify_one();
		}
};
} // namespace Detail

#define APP_DEBUG_LOG(...)     std::println(__VA_ARGS__)
#define APP_DEBUG_LOG_POP_LINE std::cout << "\33[2K\r" << std::flush;
#define APP_DEBUG_LOG_RED_PUSH std::cout << "\033[31m";
#define APP_DEBUG_LOG_RED_POP  std::cout << "\033[0m" << std::endl;
#define APP_DEBUG_LOG_RED_LINE(...)                                                                \
	APP_DEBUG_LOG_RED_PUSH                                                                         \
	std::println(__VA_ARGS__);                                                                     \
	APP_DEBUG_LOG_RED_POP

#define APP_DEBUG_LOG_SPAM(...)                                                                    \
	Detail::AsyncSpamLogger::get_instance().push_log(std::format(__VA_ARGS__))

#else
#define APP_DEBUG_LOG(...)      (static_cast<void>(0))
#define APP_DEBUG_LOG_SPAM(...) (static_cast<void>(0))
#endif