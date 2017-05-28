// Almost all the code for this recipe
// is in this header file






#include <boost/thread/thread.hpp>

namespace detail {

template <class T>
struct task_wrapped {
private:
    T task_unwrapped_;

public:
    explicit task_wrapped(const T& f)
        : task_unwrapped_(f)
    {}

    void operator()() const {
        // Resetting interruption.
        try {
            boost::this_thread::interruption_point();
        } catch(const boost::thread_interrupted&){}

        try {
            // Executing task.
            task_unwrapped_();
        } catch (const std::exception& e) {
            std::cerr<< "Exception: " << e.what() << '\n';
        } catch (const boost::thread_interrupted&) {
            std::cerr<< "Thread interrupted\n";
        } catch (...) {
            std::cerr<< "Unknown exception\n";
        }
    }
};

} // namespace detail

namespace detail {

template <class T>
task_wrapped<T> make_task_wrapped(const T& task_unwrapped) {
    return task_wrapped<T>(task_unwrapped);
}

} // namespace detail


#include <boost/asio/io_service.hpp>
namespace tp_base {

class tasks_processor: private boost::noncopyable {
protected:
    static boost::asio::io_service& get_ios() {
        static boost::asio::io_service ios;
        static boost::asio::io_service::work work(ios);

        return ios;
    }

public:
    template <class T>
    static void push_task(const T& task_unwrapped) {
        get_ios().post(detail::make_task_wrapped(task_unwrapped));
    }

    static void start() {
        get_ios().run();
    }

    static void stop() {
        get_ios().stop();
    }
}; // tasks_processor

} // namespace tp_base


#include <boost/asio/io_service.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/system/error_code.hpp>
#include <memory>  // std::unique_ptr
#include <iostream>

namespace detail {

    template <class Functor>
    struct timer_task {
    private:
        std::unique_ptr<boost::asio::deadline_timer> timer_;
        task_wrapped<Functor> task_;

    public:
        explicit timer_task(
                std::unique_ptr<boost::asio::deadline_timer> timer,
                const Functor& task_unwrapped)
            : timer_(std::move(timer))
            , task_(task_unwrapped)
        {}

        void operator()(const boost::system::error_code& error) const {
            if (!error) {
                task_();
            } else {
                std::cerr << error << '\n';
            }
        }
    };

} // namespace detail

namespace tp_timers {

class tasks_processor: public tp_base::tasks_processor {
public:

    template <class Time, class Functor>
    static void run_delayed(Time duration_or_time, const Functor& f) {
        std::unique_ptr<boost::asio::deadline_timer> timer(
            new boost::asio::deadline_timer(
                get_ios(), duration_or_time
            )
        );

        boost::asio::deadline_timer& timer_ref = *timer;

        timer_ref.async_wait(
            detail::timer_task<Functor>(
                std::move(timer),
                f
            )
        );
    }
};

} // namespace tp_timers


#include <boost/asio/ip/tcp.hpp>
#include <boost/core/noncopyable.hpp>

struct connection_with_data: boost::noncopyable {
    boost::asio::ip::tcp::socket socket;
    std::string data;

    explicit connection_with_data(boost::asio::io_service& ios)
        : socket(ios) 
    {}

    void shutdown() {
        if (!socket.is_open()) {
            return;
        }

        boost::system::error_code ignore;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignore);
        socket.close(ignore);
    }

    ~connection_with_data() {
        shutdown();
    }
};


#include <memory> // std::unique_ptr

typedef std::unique_ptr<connection_with_data> connection_ptr;


template <class T>
struct task_wrapped_with_connection {
private:
    connection_ptr c_;
    T task_unwrapped_;

public:
    explicit task_wrapped_with_connection(connection_ptr&& c, const T& f)
        : c_(std::move(c))
        , task_unwrapped_(f)
    {}

    void operator()(const boost::system::error_code& error, std::size_t bytes_count) {
        const auto lambda = [this, &error, bytes_count]() {
            this->c_->data.resize(bytes_count);
            this->task_unwrapped_(std::move(this->c_), error);
        };

        const auto task = detail::make_task_wrapped(lambda);

        task();
    }
};

#include <boost/asio/write.hpp>

template <class T>
struct task_wrapped_with_connection;

template <class Functor>
void async_write_data(connection_ptr&& c, const Functor& f) {
    boost::asio::ip::tcp::socket& s = c->socket;
    std::string& d = c->data;

    boost::asio::async_write(
        s,
        boost::asio::buffer(d),
        task_wrapped_with_connection<Functor>(std::move(c), f)
    );
}

#include <boost/asio/read.hpp>

template <class Functor>
void async_read_data(connection_ptr&& c, const Functor& f, std::size_t at_least_bytes) {
    c->data.resize(at_least_bytes);

    boost::asio::ip::tcp::socket& s = c->socket;
    std::string& d = c->data;
    char* p = (d.empty() ? 0 : &d[0]);

    boost::asio::async_read(
        s,
        boost::asio::buffer(p, d.size()),
        task_wrapped_with_connection<Functor>(std::move(c), f)
    );
}


template <class Functor>
void async_read_dataat_least(connection_ptr&& c, const Functor& f, std::size_t at_least_bytes, std::size_t at_most = 4095) {
    std::string& d = c->data;
    d.resize(at_most);
    char* p = (at_most == 0 ? 0 : &d[0]);

    boost::asio::ip::tcp::socket& s = c->socket;

    boost::asio::async_read(
        s,
        boost::asio::buffer(p, at_most),
        boost::asio::transfer_at_least(at_least_bytes),
        task_wrapped_with_connection<Functor>(std::move(c), f)
    );
}

namespace tp_network_client {

class tasks_processor: public tp_timers::tasks_processor {
    // ...

public:
    static connection_ptr create_connection(const char* addr, unsigned short port_num) {
        connection_ptr c( new connection_with_data(get_ios()) );

        c->socket.connect(boost::asio::ip::tcp::endpoint(
            boost::asio::ip::address_v4::from_string(addr),
            port_num
        ));

        return c;
    }
};

} // namespace tp_network_cleint


#include <boost/function.hpp>
namespace tp_network {

class tasks_processor: public tp_network_client::tasks_processor {
    typedef boost::asio::ip::tcp::acceptor acceptor_t;
    typedef boost::function<void(connection_ptr, const boost::system::error_code&)> on_accpet_func_t;

    struct tcp_listener {
        acceptor_t              acceptor_;
        const on_accpet_func_t  func_;
        connection_ptr          new_c_;

        template <class Functor>
        tcp_listener(
                boost::asio::io_service& io_service,
                unsigned short port,
                const Functor& task_unwrapped)
            : acceptor_(io_service, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::tcp::v4(), port
            ))
            , func_(task_unwrapped)
        {}
    };
    typedef std::unique_ptr<tcp_listener> listener_ptr;

    struct handle_accept {
        listener_ptr listener;

        explicit handle_accept(listener_ptr&& l)
            : listener(std::move(l))
        {}

        void operator()(const boost::system::error_code& error) {
            task_wrapped_with_connection<on_accpet_func_t> task(std::move(listener->new_c_), listener->func_);
            if (error) {
                std::cerr << error << '\n';
            }

            start_accepting_connection(std::move(listener));
            task(error, 0);
        }
    };

   static void start_accepting_connection(listener_ptr&& listener) {
        if (!listener->acceptor_.is_open()) {
            return;
        }

        listener->new_c_.reset(
            new connection_with_data(listener->acceptor_.get_io_service())
        );

        boost::asio::ip::tcp::socket& s = listener->new_c_->socket;
        acceptor_t& a = listener->acceptor_;
        a.async_accept(
            s,
            tasks_processor::handle_accept(std::move(listener))
        );
    }

public:
    template <class Functor>
    static void add_listener(unsigned short port_num, const Functor& f) {
        std::unique_ptr<tcp_listener> listener(
            new tcp_listener(get_ios(), port_num, f)
        );

        start_accepting_connection(std::move(listener));
    }

    static connection_ptr create_connection(const char* addr, unsigned short port_num) {
        connection_ptr c( new connection_with_data(get_ios()) );

        c->socket.connect(boost::asio::ip::tcp::endpoint(
            boost::asio::ip::address_v4::from_string(addr),
            port_num
        ));

        return c;
    }
};

} // namespace tp_network


#include <boost/thread.hpp>

namespace tp_multithread {

class tasks_processor: public tp_network::tasks_processor {
public:
    // Default value will attempt to guess optimal count of threads.
    static void start_multiple(std::size_t threads_count = 0) {
        if (!threads_count) {
            threads_count = (std::max)(static_cast<int>(
                boost::thread::hardware_concurrency()), 1
            );
        }

        // one thread is the current thread
        -- threads_count;

        boost::asio::io_service& ios = get_ios();
        boost::thread_group tg;
        for (std::size_t i = 0; i < threads_count; ++i) {
            tg.create_thread([&ios]() { ios.run(); });
        }

        ios.run();
        tg.join_all();
    }
};

} // namespace tp_multithread



#include <boost/asio/signal_set.hpp>
#include <boost/function.hpp>

namespace tp_full {

class tasks_processor: public tp_multithread::tasks_processor {
protected:
    static boost::asio::signal_set& signals() {
        static boost::asio::signal_set signals_(get_ios());
        return signals_;
    }

    static boost::function<void(int)>& signal_handler() {
        static boost::function<void(int)> users_signal_handler_;
        return users_signal_handler_;
    }

private:
    static void handle_signals(
            const boost::system::error_code& error,
            int signal_number)
    {
        signals().async_wait(&tasks_processor::handle_signals);

        if (error) {
            std::cerr << "Error in signal handling: " << error << '\n';
        } else {
            // If signals occures while there is no waiting handlers,
            // signal notification is queued, so it won't be missed
            // while we running users_signal_handler_
            boost::function<void(int)> h = signal_handler();
            detail::make_task_wrapped([h, signal_number]() {
                h(signal_number);
            })(); // make and run task_wrapped
        }

    }
public:

    // This function is not threads safe!
    // Must be called before all the `start()` calls
    // Function can be called only once
    template <class Func>
    static void register_signals_handler(
            const Func& f,
            const std::vector<int>& signals_to_wait)
    {

        // Making shure that this is the first call
        assert(!signal_handler()); 

        signal_handler() = f;
        boost::asio::signal_set& sigs = signals();
        
        std::for_each(
            signals_to_wait.begin(),
            signals_to_wait.end(),
            [&sigs](int signal) { sigs.add(signal); }
        );

        signals().async_wait(&tasks_processor::handle_signals);
    }
};

} // namespace tp_full


using namespace tp_full;

void accept_3_signals_and_stop(int signal) {
    static int signals_count = 0;
    assert(signal == SIGINT);
    ++ signals_count;
    std::cout << "Captured " << signals_count << " SIGINT\n"; 
    if (signals_count == 3) {
        tasks_processor::stop();
    }
}


int main () {
    tasks_processor::register_signals_handler(
        &accept_3_signals_and_stop,
        std::vector<int>(1, SIGINT) // vector containing 1 element
    );

    tasks_processor::start();
}