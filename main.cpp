#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <stdexcept>

using Fiber = std::function<void()>;

class Scheduler {
public:
    virtual ~Scheduler() = default;

    virtual void schedule(Fiber fiber) = 0;

    virtual void yield() = 0;

    virtual void run() = 0;
};

std::unique_ptr<Scheduler> global_scheduler;

void schedule(Fiber fiber) {
    if (!global_scheduler) {
        throw std::runtime_error("Global scheduler is empty");
    }
    global_scheduler->schedule(std::move(fiber));
}

void yield() {
    if (!global_scheduler) {
        throw std::runtime_error("Global scheduler is empty");
    }
    global_scheduler->yield();
}

class InlineScheduler : public Scheduler {
public:
    void schedule(Fiber fiber) override {
        fiber();
    }

    void yield() override {
    }

    void run() override {
    }
};

class StackPool {
public:
    enum {
        STACK_SIZE = 1024 * 1024,
    };

    ~StackPool() {
        for (auto elem : stacks) {
            ::free(elem);
        }
    }

    void *alloc() {
        if (!stacks.empty()) {
            auto ptr = stacks.back();
            stacks.pop_back();
            return ptr;
        }
        auto ptr = calloc(1, STACK_SIZE);
        return ptr;
    }

    void free(void *stack) {
        stacks.push_back(stack);
    }

private:
    std::vector<void *> stacks;
} stack_pool;

class FiberScheduler : public Scheduler {
    struct Context {
        intptr_t eip = 0;
        intptr_t esp = 0;

        void switch_context() {
            auto *eipp = &eip;
            auto *espp = &esp;
            asm volatile("push %%ebp\n\t"

                        "xchgl %%esp, (%0)\n\t"  

                        "movl (%1), %%eax\n\t"
                        "movl $ret_label%=, (%1)\n\t"  

                        "call *%%eax\n\t"
                        
                        "ret_label%=:\n\t"

                        "pop %%ebp\n\t"
                        "pop %%ebp\n\t"
                        : "+S"(espp), "+D"(eipp)::"eax", "ebx", "ecx", "edx", "memory",
                        "cc");
        }
    };

    void run_one() {
        assert(!queue.empty());

        main_loop = queue.front();
        queue.pop();

        main_loop.switch_context();
    }

    struct FiberKeeper {
        Fiber fiber;
        void *stack;

        explicit FiberKeeper(Fiber fiber)
            : fiber(std::move(fiber)), stack(stack_pool.alloc()) {
        }

        FiberKeeper(FiberKeeper &&other) : fiber(std::move(other.fiber)) {
            stack = other.stack;
            other.stack = nullptr;
        }

        FiberKeeper(const FiberKeeper &other) = delete;
        void operator=(FiberKeeper &&other) = delete;
        void operator=(const FiberKeeper &other) = delete;

        ~FiberKeeper() {
            if (stack) {
                stack_pool.free(stack);
            }
        }
    };

    static void run_fiber(FiberKeeper fiber) {
        fiber.fiber();

        fiber.~FiberKeeper();

        main_loop.switch_context();
        __builtin_unreachable();
    }

public:
    ~FiberScheduler() override {
        assert(queue.empty());
    }

    void schedule(Fiber fiber) override {
        FiberKeeper fk(std::move(fiber));
        Context context = Context();

        intptr_t start_of_stack = (intptr_t)fk.stack + StackPool::STACK_SIZE;

        intptr_t memory_for_fk = start_of_stack - (intptr_t)sizeof(fk);
        memcpy((void *)memory_for_fk, (void *)(&fk), sizeof(fk));

        void *fk_ptr = (struct FiberKeeper *)memory_for_fk;
        intptr_t memory_for_ptr = memory_for_fk - (intptr_t)sizeof(fk_ptr);
        memcpy((void *)memory_for_ptr, &fk_ptr, sizeof(fk_ptr));

        context.esp = memory_for_ptr;
        context.eip = (intptr_t)&run_fiber;

        memset(&fk, 0, sizeof(fk));

        queue.push(context);
    }

    void yield() override {
        queue.push(main_loop);
        queue.back().switch_context();
    }

    void run() override {
        while (!queue.empty()) {
            run_one();
        }
    }

    std::queue<Context> queue;
    static Context main_loop;
};

FiberScheduler::Context FiberScheduler::main_loop;

void init_global_scheduler() {
    global_scheduler = std::make_unique<FiberScheduler>();
}

void run_global_scheduler() {
    global_scheduler->run();
}

void test_simple() {
    int x = 0;

    schedule([&]() {
        ++x;
        std::cout << "Done" << std::endl;
    });

    run_global_scheduler();

    assert(x == 1);
    std::cout << "First test accepted" << std::endl;
}

void test_multiple() {
    int x = 0;

    schedule([&]() {
        ++x;
        std::cout << "Done" << std::endl;
    });
    schedule([&]() {
        ++x;
        std::cout << "Done" << std::endl;
    });
    schedule([&]() {
        ++x;
        std::cout << "Done" << std::endl;
    });

    run_global_scheduler();

    assert(x == 3);
    std::cout << "Second test accepted" << std::endl;
}

void test_recursive() {
    int x = 0;

    schedule([&]() {
        schedule([&]() {
            ++x;
            std::cout << "Done" << std::endl;
        });
    });
    schedule([&]() {
        schedule([&]() {
            schedule([&]() {
                ++x;
                std::cout << "Done" << std::endl;
            });
        });
    });
    schedule([&]() {
        schedule([&]() {
            schedule([&]() {
                schedule([&]() {
                    ++x;
                    std::cout << "Done" << std::endl;
                });
            });
        });
    });

    run_global_scheduler();

    assert(x == 3);
    std::cout << "Third test accepted" << std::endl;
}

enum {
    ITERS = 10,
};

void test_yield_one() {
    int x = 0;

    schedule([&]() {
        for (int i = 0; i != ITERS; ++i) {
            ++x;
            yield();
        }
        std::cout << "Done" << std::endl;
    });

    assert(x == 0);

    run_global_scheduler();

    assert(x == ITERS);

    std::cout << "Forth test accepted" << std::endl;
}

void test_yield_many() {
    int x = 0;
    int cur_fiber = -1;

    auto create_fiber = [&](int fiber_id) {
        return [&, fiber_id]() {
            for (int i = 0; i != ITERS; ++i) {
                assert(cur_fiber != fiber_id);
                cur_fiber = fiber_id;
                ++x;
                yield();
            }
            std::cout << "Done" << std::endl;
        };
    };

    schedule(create_fiber(1));
    schedule(create_fiber(2));
    schedule(create_fiber(3));

    assert(x == 0);

    run_global_scheduler();

    assert(x == 3 * ITERS);

    std::cout << "Fifth test accepted" << std::endl;
}

void test_secret() {
    run_global_scheduler();
}

int main() {
    init_global_scheduler();

    test_simple();
    test_multiple();
    test_recursive();
    test_yield_one();
    test_yield_many();
    test_secret();
}
