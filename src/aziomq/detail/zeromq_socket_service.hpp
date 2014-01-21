/*
    Copyright (c) 2013-2014 Contributors as noted in the AUTHORS file

    This file is part of aziomq

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef AZIOMQ_ZEROMQ_SOCKET_SERVICE_HPP_
#define AZIOMQ_ZEROMQ_SOCKET_SERVICE_HPP_

#include "../error.hpp"
#include "scope_guard.hpp"
#include "zeromq_send_op.hpp"
#include "zeromq_receive_op.hpp"

#include <boost/assert.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/detail/reactor.hpp>
#include <boost/asio/detail/reactor_op.hpp>
#include <boost/asio/detail/socket_types.hpp>
#include <boost/system/system_error.hpp>

#include <zmq.h>

#include <string>
#include <memory>
#include <mutex>

namespace aziomq {
namespace detail {
    // TODO This is probably completely broken for Windows, if anybody cares
    // patches are welcome.
    class zeromq_socket_service : public boost::asio::io_service::service {
    public:
        using context_pointer_type = std::shared_ptr<void>; // hold the context via shared_ptr
        using socket_type = void*; // zeromq socket type
        using native_handle_type = socket_ops::native_handle_type; // underlying FD
        using reactor = boost::asio::detail::reactor;
        using endpoint_type = socket_ops::endpoint_type;
        using mutex_type  = std::mutex;

        static boost::asio::io_service::id id;

        struct implementation_type {
            socket_type socket_;
            int shutdown_;
            endpoint_type endpoint_;

            reactor::per_descriptor_data reactor_data_;
        };

        explicit zeromq_socket_service(boost::asio::io_service & io_service) :
            boost::asio::io_service::service(io_service),
            ctx_(get_context()),
            reactor_(boost::asio::use_service<reactor>(io_service)) {
                reactor_.init_task();
        }

        virtual void shutdown_service() {
            if (!ctx_) return;
            ctx_.reset();
        }

        context_pointer_type context() { return ctx_; }

        void construct(implementation_type & impl) const {
            impl.socket_ = nullptr;
            impl.shutdown_ = -1;
        }

        void move_construct(implementation_type & impl,
                            zeromq_socket_service & other_service,
                            implementation_type & other) const {
            impl.socket_ = other.socket_;
            other.socket_ = nullptr;

            impl.shutdown_ = other.shutdown_;
            other.shutdown_ = -1;

            impl.endpoint_ = other.endpoint_;
            other.endpoint_ = std::string();

            other_service.reactor_.move_descriptor(native_handle(impl),
                                        impl.reactor_data_, other.reactor_data_);
        }

        void move_assign(implementation_type & impl,
                         zeromq_socket_service & other_service,
                         implementation_type & other) const {
            destroy(impl);
            impl.socket_ = other.socket_;
            other.socket_ = nullptr;

            impl.shutdown_ = other.shutdown_;
            other.shutdown_ = -1;

            impl.endpoint_ = other.endpoint_;
            other.endpoint_ = std::string();

            other_service.reactor_.move_descriptor(native_handle(impl),
                                        impl.reactor_data_, other.reactor_data_);
        }

        boost::system::error_code do_open(implementation_type & impl,
                                          int type,
                                          boost::system::error_code & ec) {
            BOOST_ASSERT_MSG(ctx_, "Attempting to use ZeroMQ context after calling shutdown()");

            if (is_open(impl))
                return ec = make_error_code(boost::system::errc::bad_file_descriptor);

            impl.socket_ = zmq_socket(ctx_.get(), type);
            if (!impl.socket_)
                return ec = make_error_code();
            auto guard = scope_guard([&] { zmq_close(impl.socket_); });
            if (int err = reactor_.register_descriptor(native_handle(impl), impl.reactor_data_))
                return ec = make_error_code(err);
            guard.dismiss();
            AZIOMQ_TRACKED_LOG("opening socket " << std::hex << native_handle(impl));

            return ec;
        }

        void destroy(implementation_type & impl) const {
            if (!is_open(impl)) return;
            reactor_.deregister_descriptor(native_handle(impl),
                    impl.reactor_data_, true);
            boost::system::error_code ec;
            close(impl, ec);
        }

        bool is_open(const implementation_type & impl) const {
            return impl.socket_ != nullptr;
        }

        // Disable sends or receives on the socket.
        boost::system::error_code shutdown(implementation_type& impl,
                                           socket_ops::shutdown_type what,
                                           boost::system::error_code& ec) const
        {
            if (impl.socket_ == nullptr)
                return ec = make_error_code(boost::system::errc::bad_file_descriptor);

            if (what < impl.shutdown_)
               return ec = make_error_code(boost::system::errc::invalid_argument);
            impl.shutdown_ = what;
            return ec;
        }

        boost::system::error_code cancel(implementation_type & impl, boost::system::error_code & ec) const {
            if (!is_open(impl))
                return ec = make_error_code(boost::system::errc::bad_file_descriptor);
            reactor_.cancel_ops(native_handle(impl), impl.reactor_data_);
            return ec = boost::system::error_code();
        }

        boost::system::error_code close(implementation_type & impl, boost::system::error_code & ec) const {
            BOOST_ASSERT_MSG(impl.socket_, "Invalid socket");

            ec = boost::system::error_code();
            auto rc = zmq_close(impl.socket_);
            if (rc)
                ec = make_error_code();
            impl.socket_ = nullptr;
            return ec;
        }

        /* \brief set an option on the underlying zeromq context
         * \tparam Option option type
         * \param option Option to set
         */
        template<typename Option>
        boost::system::error_code set_option(const Option & option, boost::system::error_code & ec) {
            if (ctx_ == nullptr) {
                ec = make_error_code(EINVAL);
            } else {
                auto rc = zmq_ctx_set(ctx_, option.name(), option.data());
                if (!rc)
                    ec = make_error_code();
            }
            return ec;
        }

        template<typename Option>
        boost::system::error_code set_option(implementation_type & impl,
                const Option & option, boost::system::error_code & ec) {
            return socket_ops::set_option(impl.socket_, option, ec);
        }

        template<typename Option>
        boost::system::error_code get_option(Option & option,
                                             boost::system::error_code & ec) {
            if (ctx_ == nullptr) {
                ec = make_error_code(EINVAL);
            } else {
                auto rc = zmq_ctx_get(ctx_, option.name());
                if (rc < 0)
                    ec = make_error_code();
                else
                    option.set(rc);
            }
            return ec;
        }

        template<typename Option>
        static boost::system::error_code get_option(implementation_type & impl,
                Option & option, boost::system::error_code & ec) {
            return socket_ops::get_option(impl.socket_, option, ec);
        }

        native_handle_type native_handle(const implementation_type & impl) const {
            native_handle_type res;
            boost::system::error_code ec;
            ec = socket_ops::native_handle(impl.socket_, res, ec);
            if (ec)
                throw boost::system::system_error(ec);
            return res;
        }

        endpoint_type endpoint(const implementation_type & impl) const { return impl.endpoint_; }

        boost::system::error_code bind(implementation_type & impl,
                                       const endpoint_type & endpoint,
                                       boost::system::error_code & ec) {
            ec = check_endpoint(impl, ec);
            if (ec)
                return ec;

            ec = socket_ops::bind(impl.socket_, endpoint, ec);
            if (ec)
                return ec;

            impl.endpoint_ = endpoint;
            return ec;
        }

        boost::system::error_code connect(implementation_type & impl,
                                          const endpoint_type & endpoint,
                                          boost::system::error_code & ec) {
            ec = check_endpoint(impl, ec);
            if (ec)
                return ec;

            ec = socket_ops::connect(impl.socket_, endpoint, ec);
            if (ec)
                return ec;

            impl.endpoint_ = endpoint;
            return ec;
        }

        template<typename ConstBufferSequence>
        size_t send(implementation_type & impl,
                    const ConstBufferSequence & buffers,
                    int flags,
                    boost::system::error_code & ec) {
            size_t bytes_transferred = 0;
            try {
                message msg;
                if (flags & ZMQ_SNDMORE) {
                    auto rc = socket_ops::send(msg, impl.socket_, buffers, flags, socket_ops::send_more_t());
                    bytes_transferred = rc.get();
                } else {
                    auto rc = socket_ops::send(msg, impl.socket_, buffers, flags);
                    bytes_transferred = rc.get();
                }
            } catch (const boost::system::system_error & e) {
                ec = e.code();
            }
            return bytes_transferred;
        }

        template<typename ConstBufferSequence,
                 typename Handler>
        void async_send(implementation_type & impl,
                        const ConstBufferSequence & buffers,
                        Handler handler,
                        int flags) {
            bool is_continuation = boost_asio_handler_cont_helpers::is_continuation(handler);

            typedef zeromq_send_op<ConstBufferSequence, Handler> op;
            typename op::ptr p = { boost::asio::detail::addressof(handler),
                boost_asio_handler_alloc_helpers::allocate(sizeof(op), handler), 0 };
            p.p = new (p.v) op(impl.socket_, buffers, handler, flags | ZMQ_DONTWAIT);

            start_op(impl, boost::asio::detail::reactor::write_op, p.p,
                        is_continuation, true);
            p.v = p.p = 0;
        }

        template<typename MutableBufferSequence>
        size_t receive(implementation_type & impl,
                       const MutableBufferSequence & buffers,
                       int flags,
                       boost::system::error_code & ec) {
            size_t bytes_transferred = 0;
            try {
                message msg;
                if (flags & ZMQ_RCVMORE) {
                    auto rc = socket_ops::receive(msg, impl.socket_, buffers, flags,
                                                    socket_ops::receive_more_t());
                    bytes_transferred = rc.get();
                } else {
                    auto rc = socket_ops::receive(msg, impl.socket_, buffers, flags);
                    bytes_transferred = rc.get();
                }
            } catch (const boost::system::system_error & e) {
                ec = e.code();
            }
            return bytes_transferred;
        }

        template<typename MutableBufferSequence,
                 typename Handler>
        void async_receive(implementation_type & impl,
                           const MutableBufferSequence & buffers,
                           Handler handler,
                           int flags) {
            bool is_continuation = boost_asio_handler_cont_helpers::is_continuation(handler);

            typedef zeromq_receive_op<MutableBufferSequence, Handler> op;
            typename op::ptr p = { boost::asio::detail::addressof(handler),
                boost_asio_handler_alloc_helpers::allocate(sizeof(op), handler), 0 };
            p.p = new (p.v) op(impl.socket_, buffers, handler, flags);

            start_op(impl, boost::asio::detail::reactor::read_op, p.p,
                        is_continuation, true);
            p.v = p.p = 0;
        }

        static mutex_type & static_mutex();

    private:
        static boost::system::error_code check_endpoint(const implementation_type & impl,
                                                        boost::system::error_code & ec) {
            return impl.endpoint_.empty() ? ec :
                                            make_error_code(boost::system::errc::already_connected);
        }

        // retrieve shared context pointer
        static context_pointer_type get_context();

        void start_op(implementation_type& impl, int op_type,
                      boost::asio::detail::reactor_op* op,
                      bool is_continuation, bool is_non_blocking) {
            if (reactor_op::is_noop(op)) {
                reactor_.post_immediate_completion(op, is_continuation);
                return;
            }
            reactor_.start_op(op_type, native_handle(impl),
                    impl.reactor_data_, op, is_continuation, is_non_blocking);
        }
        context_pointer_type ctx_;
        reactor & reactor_;
    };
} // namespace detail
} // namespace aziomq

// #include <boost/asio/detail/impl/epoll_reactor.ipp>

#endif // AZIOMQ_ZEROMQ_SOCKET_SERVICE_HPP_


