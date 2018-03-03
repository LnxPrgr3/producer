#include "message_queue/message_queue.h"
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>

class step {
public:
	template <typename Operation>
	step(Operation &&operation, std::string name, std::string previous, std::string next,
	     size_t blockers)
	    : _operation(std::forward<Operation>(operation)), _name(name), _previous(previous),
	      _next(next), _blockers(blockers), _unblocked(0) {}

	void add_blocking(step *step) { _blocking.push_back(step); }
	bool unblock() { return __sync_add_and_fetch(&_unblocked, 1) >= _blockers; }

	const std::function<void(step *)> &operation() const { return _operation; }
	const std::string &name() const { return _name; }
	const std::string &previous() const { return _previous; }
	const std::string &next() const { return _next; }
	const std::vector<step *> &blocking() const { return _blocking; }
	size_t blockers() const { return _blockers; }

private:
	std::function<void(step *)> _operation;
	std::string _name;
	std::string _previous;
	std::string _next;
	std::vector<step *> _blocking;
	size_t _blockers;
	size_t _unblocked;
};

using step_list = std::vector<step>;

class context {
public:
	context(int cpus) {
		message_queue_init(&dispatch, sizeof(step *), cpus);
		message_queue_init(&backchannel, sizeof(step *), cpus);
	}

	~context() {
		message_queue_destroy(&backchannel);
		message_queue_destroy(&dispatch);
	}

	step *get_unblocked() {
		auto unblocked_ptr =
		    reinterpret_cast<class step **>(message_queue_tryread(&backchannel));
		step *unblocked = nullptr;
		if (unblocked_ptr)
			unblocked = handle_unblocked(unblocked_ptr);
		return unblocked;
	}

	step *wait_for_unblocked() { return handle_unblocked(message_queue_read(&backchannel)); }

	void queue(step *step) {
		auto message =
		    reinterpret_cast<class step **>(message_queue_message_alloc_blocking(&dispatch));
		*message = step;
		message_queue_write(&dispatch, message);
	}

	bool unblock(step *step) {
		auto message =
		    reinterpret_cast<class step **>(message_queue_message_alloc(&backchannel));
		if (message) {
			*message = step;
			message_queue_write(&backchannel, message);
			return true;
		}
		return false;
	}

	step **wait_for_step() { return reinterpret_cast<step **>(message_queue_read(&dispatch)); }

	void free_step(step **step) { message_queue_message_free(&dispatch, step); }

private:
	message_queue dispatch;
	message_queue backchannel;

	step *handle_unblocked(void *message) {
		auto unblocked_ptr = reinterpret_cast<class step **>(message);
		step *unblocked = *unblocked_ptr;
		message_queue_message_free(&backchannel, unblocked_ptr);
		return unblocked;
	}
};

static std::string replace_end(std::string x, const std::string &end) {
	if (x.empty())
		return x;
	size_t offset = x.size() - end.size();
	for (size_t i = 0; i < end.size(); ++i) {
		x[i + offset] = end[i];
	}
	return x;
}

static void print_command(std::vector<const char *> command) {
	std::stringstream str;
	for (size_t i = 0; i < command.size(); ++i) {
		if (i)
			str << " ";
		bool quote = strchr(command[i], ' ');
		if (quote)
			str << "\"";
		str << command[i];
		if (quote)
			str << "\"";
	}
	str << std::endl;
	std::cout << str.str();
}

static void convert_to_intermediate(step *step) {
	// generic setup
	std::vector<const char *> argv{"afconvert", step->name().c_str()};
	if (step->previous().size()) {
		argv.push_back("--gapless-before");
		argv.push_back(step->previous().c_str());
	}
	if (step->next().size()) {
		argv.push_back("--gapless-after");
		argv.push_back(step->next().c_str());
	}
	// push command-specific args
	const char *args[] = {
	    "-d",   "LEF32@44100", "-f", "caff", "--soundcheck-generate", "--src-complexity",
	    "bats", "-r",          "127"};
	argv.insert(argv.end(), args, args + 9);
	// push destination file
	auto destination = replace_end(step->name(), "caf");
	argv.push_back(destination.c_str());
	print_command(argv);
	// fork, exec, and wait
	argv.push_back(nullptr);
	pid_t pid = fork();
	if (pid < 0) {
		perror("Error spawning child process");
		exit(-1);
	} else if (pid == 0) {
		execvp(argv[0], const_cast<char *const *>(argv.data()));
	} else {
		int status;
		waitpid(pid, &status, 0);
	}
}

static void convert_to_output(step *step) {
	// generic setup
	std::vector<const char *> argv{"afconvert", step->name().c_str()};
	if (step->previous().size()) {
		argv.push_back("--gapless-before");
		argv.push_back(step->previous().c_str());
	}
	if (step->next().size()) {
		argv.push_back("--gapless-after");
		argv.push_back(step->next().c_str());
	}
	// push command-specific args
	const char *args[] = {"-d", "aac",    "-f", "m4af", "-u", "pgcm", "2", "--soundcheck-read",
	                      "-b", "256000", "-q", "127",  "-s", "2"};
	argv.insert(argv.end(), args, args + 14);
	// push destination file
	auto destination = replace_end(step->name(), "m4a");
	argv.push_back(destination.c_str());
	print_command(argv);
	// fork, exec, and wait
	argv.push_back(nullptr);
	pid_t pid = fork();
	if (pid < 0) {
		perror("Error spawning child process");
		exit(-1);
	} else if (pid == 0) {
		execvp(argv[0], const_cast<char *const *>(argv.data()));
	} else {
		int status;
		waitpid(pid, &status, 0);
	}
}

static step_list plan(int argc, char *argv[]) {
	std::string previous, name, next;
	step_list steps;
	std::map<std::string, step *> steps_by_name;
	steps.reserve((argc - 1) * 2 + 1);
	for (int i = 1; i < argc + 1; ++i) {
		previous = std::move(name);
		name = std::move(next);
		next = std::string(i < argc ? argv[i] : "");
		if (name.size()) {
			steps_by_name.emplace(name, &steps.emplace_back(convert_to_intermediate, name,
			                                                previous, next, 0));
		}
	}
	previous.clear();
	name.clear();
	next.clear();
	for (int i = 1; i < argc + 1; ++i) {
		previous = std::move(name);
		name = std::move(next);
		next = std::string(i < argc ? argv[i] : "");
		if (name.size()) {
			size_t blockers = 1 + (previous.size() ? 1 : 0) + (next.size() ? 1 : 0);
			step *step = &steps.emplace_back(convert_to_output, replace_end(name, "caf"),
			                                 replace_end(previous, "caf"),
			                                 replace_end(next, "caf"), blockers);
			steps_by_name[name]->add_blocking(step);
			if (previous.size())
				steps_by_name[previous]->add_blocking(step);
			if (next.size())
				steps_by_name[next]->add_blocking(step);
		}
	}
	step *step = &steps.emplace_back(nullptr, "", "", "", argc - 1);
	for (int i = 0; i < argc - 1; ++i) {
		steps[argc - 1 + i].add_blocking(step);
	}
	return steps;
}

extern "C" void *worker(void *data) {
	context &context = *reinterpret_cast<class context *>(data);
	std::deque<step *> unblocked;
	while (true) {
		auto step = context.wait_for_step();
		if (!*step) {
			context.free_step(step);
			return data;
		}
		(*step)->operation()(*step);
		for (auto blocking : (*step)->blocking()) {
			if (blocking->unblock())
				unblocked.push_back(blocking);
		}
		while (unblocked.size()) {
			auto blocking = unblocked.front();
			if (!context.unblock(blocking))
				break;
			unblocked.pop_front();
		}
		context.free_step(step);
	}
}

static int32_t count_cpus() {
	int32_t count = 0;
	size_t len = sizeof(count);
	if (sysctlbyname("hw.physicalcpu", &count, &len, nullptr, 0)) {
		count = 2;
	}
	return count;
}

int main(int argc, char *argv[]) {
	auto steps = plan(argc, argv);
	const int32_t cpus = count_cpus();
	context context(cpus);
	pthread_t workers[cpus];
	for (int32_t i = 0; i < cpus; ++i) {
		pthread_create(&workers[i], NULL, &worker, &context);
	}
	for (auto &step : steps) {
		if (step.blockers())
			break;
		while (auto unblocked = context.get_unblocked()) {
			context.queue(unblocked);
		}
		context.queue(&step);
	}
	while (auto unblocked = context.wait_for_unblocked()) {
		if (unblocked->operation()) {
			context.queue(unblocked);
		} else {
			for (int32_t i = 0; i < cpus; ++i) {
				context.queue(nullptr);
			}
			break;
		}
	}
	for (int32_t i = 0; i < cpus; ++i) {
		pthread_join(workers[i], nullptr);
	}
}
