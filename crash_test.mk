# This file is used by Meta-internal infrastructure as well as by Makefile

# When included from Makefile, there are rules to build DB_STRESS_CMD. When
# used directly with `make -f crashtest.mk ...` there will be no rules to
# build DB_STRESS_CMD so it must exist prior.
DB_STRESS_CMD?=./db_stress

include python.mk

CRASHTEST_MAKE=$(MAKE) -f crash_test.mk
CRASHTEST_PY=$(PYTHON) -u tools/db_crashtest.py --stress_cmd=$(DB_STRESS_CMD)

.PHONY: crash_test crash_test_with_atomic_flush crash_test_with_txn \
	crash_test_with_best_efforts_recovery crash_test_with_ts \
	blackbox_crash_test blackbox_crash_test_with_atomic_flush \
	blackbox_crash_test_with_txn blackbox_crash_test_with_ts \
	blackbox_crash_test_with_best_efforts_recovery \
	whitebox_crash_test whitebox_crash_test_with_atomic_flush \
	whitebox_crash_test_with_txn whitebox_crash_test_with_ts

crash_test: $(DB_STRESS_CMD)
# Do not parallelize
	$(CRASHTEST_MAKE) whitebox_crash_test
	$(CRASHTEST_MAKE) blackbox_crash_test

crash_test_with_atomic_flush: $(DB_STRESS_CMD)
# Do not parallelize
	$(CRASHTEST_MAKE) whitebox_crash_test_with_atomic_flush
	$(CRASHTEST_MAKE) blackbox_crash_test_with_atomic_flush

crash_test_with_txn: $(DB_STRESS_CMD)
# Do not parallelize
	$(CRASHTEST_MAKE) whitebox_crash_test_with_txn
	$(CRASHTEST_MAKE) blackbox_crash_test_with_txn

crash_test_with_best_efforts_recovery: blackbox_crash_test_with_best_efforts_recovery

crash_test_with_ts: $(DB_STRESS_CMD)
# Do not parallelize
	$(CRASHTEST_MAKE) whitebox_crash_test_with_ts
	$(CRASHTEST_MAKE) blackbox_crash_test_with_ts

blackbox_crash_test: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --simple blackbox $(CRASH_TEST_EXT_ARGS)
	$(CRASHTEST_PY) blackbox $(CRASH_TEST_EXT_ARGS)

blackbox_crash_test_with_atomic_flush: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --cf_consistency blackbox $(CRASH_TEST_EXT_ARGS)

blackbox_crash_test_with_txn: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --txn blackbox $(CRASH_TEST_EXT_ARGS)

blackbox_crash_test_with_best_efforts_recovery: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --test_best_efforts_recovery blackbox $(CRASH_TEST_EXT_ARGS)

blackbox_crash_test_with_ts: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --enable_ts blackbox $(CRASH_TEST_EXT_ARGS)

ifeq ($(CRASH_TEST_KILL_ODD),)
  CRASH_TEST_KILL_ODD=888887
endif

whitebox_crash_test: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --simple whitebox --random_kill_odd \
      $(CRASH_TEST_KILL_ODD) $(CRASH_TEST_EXT_ARGS)
	$(CRASHTEST_PY) whitebox  --random_kill_odd \
      $(CRASH_TEST_KILL_ODD) $(CRASH_TEST_EXT_ARGS)

whitebox_crash_test_with_atomic_flush: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --cf_consistency whitebox  --random_kill_odd \
      $(CRASH_TEST_KILL_ODD) $(CRASH_TEST_EXT_ARGS)

whitebox_crash_test_with_txn: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --txn whitebox --random_kill_odd \
      $(CRASH_TEST_KILL_ODD) $(CRASH_TEST_EXT_ARGS)

whitebox_crash_test_with_ts: $(DB_STRESS_CMD)
	$(CRASHTEST_PY) --enable_ts whitebox --random_kill_odd \
      $(CRASH_TEST_KILL_ODD) $(CRASH_TEST_EXT_ARGS)
