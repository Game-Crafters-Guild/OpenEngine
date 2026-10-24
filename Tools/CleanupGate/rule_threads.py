"""Class 06, threading: threads, futures, condition variables, sleeping polls and custom pools outside the job system.

The owners (JobSystem, Platform) are out of scope. A line counts once, for the first pattern it matches.
"""
import re

from gate_common import Site, strip_strings

CLASS_ID = "06_threading"

kPatterns = [
    ("std_thread", re.compile(r"\bstd::(?:thread|jthread)\b(?!::)"), "a job (JobChannel::Submit or Enqueue); a long-lived loop becomes a repeating job or a Scheduler entry"),
    ("std_thread_id_info", re.compile(r"\bstd::thread::id\b"), "a thread-affinity check: allowlist it when it is one"),
    ("std_async", re.compile(r"\bstd::async\b"), "JobChannel::Submit returning a TaskHandle; JobCounter Run and Wait for fork-join"),
    ("future_promise", re.compile(r"\bstd::(?:future|promise|packaged_task|shared_future)\b"), "TaskHandle and Task::PublishResult (JobSystem)"),
    ("condition_variable", re.compile(r"\bstd::condition_variable(?:_any)?\b"), "JobCounter::Wait or a continuation (Task::AddDependency); a worker queue becomes a JobChannel"),
    ("sleep_poll", re.compile(r"\bsleep_for\s*\(|\bsleep_until\s*\(|\bstd::this_thread::yield\s*\(|\bSleep\s*\(\s*\d|\busleep\s*\(|\bnanosleep\s*\("), "a repeating job (Scheduler) or a wait on a JobCounter, never a sleeping poll"),
    ("native_thread", re.compile(r"\bCreateThread\s*\(|\b_beginthreadex\s*\(|\bpthread_create\s*\(|\bdispatch_async\s*\(|\bdispatch_queue_create\s*\("), "a job, unless the platform API requires a dedicated thread"),
    ("thread_pool_custom", re.compile(r"\b(?:class|struct)\s+(?!WorkStealingThreadPool\b)\w*(?:ThreadPool|WorkerPool|Worker|WorkQueue|JobQueue)\b(?!\s*;)"), "JobSystem (WorkStealingThreadPool, JobChannel)"),
    ("atomic_spin", re.compile(r"while\s*\([^;]*\.load\s*\([^)]*\)[^;]*\)\s*(?:\{\s*\})?;?\s*$|while\s*\(\s*!\s*\w+\.load\("), "JobCounter::Wait or a continuation"),
    ("latch_barrier", re.compile(r"\bstd::(?:latch|barrier|counting_semaphore|binary_semaphore)\b"), "JobCounter"),
]
PATTERNS = [name for name, _, _ in kPatterns]
kExcludedModules = {"JobSystem", "Platform"}


class Rule:
    def __init__(self, files):
        pass

    def scan_file(self, source):
        if source.module in kExcludedModules:
            return []
        sites = []
        for line_number, code, _ in source.code:
            if not code.strip() or code.lstrip().startswith("#include") or "hardware_concurrency" in code:
                continue
            code = strip_strings(code)
            for name, pattern, piece in kPatterns:
                if pattern.search(code):
                    sites.append(Site(source.path, line_number, name, piece))
                    break
        return sites

    def finish(self):
        return []
