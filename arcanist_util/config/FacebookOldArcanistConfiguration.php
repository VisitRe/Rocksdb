<?php
// Copyright 2004-present Facebook. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

require('RocksDBCommonHelper.php');

define("DIFF_COMMAND", "diff");

class FacebookArcanistConfiguration extends ArcanistConfiguration {
  public function getCustomArgumentsForCommand($command) {
    if ($command == "land") {
      return array(
          'async' => array('help' => 'Just to make tools happy'));
    }
    return array();
  }

  public function didRunWorkflow($command,
                                 ArcanistBaseWorkflow $workflow,
                                 $error_code) {
    // Default options don't terminate on failure, but that's what we want. In
    // the current case we use assertions intentionally as "terminate on failure
    // invariants".
    assert_options(ASSERT_BAIL, true);

    assert($workflow);
    assert(strlen($command) > 0);

    if ($command == DIFF_COMMAND && !$workflow->isRawDiffSource()) {
      $diffID = $workflow->getDiffId();

      // When submitting a diff this code path gets executed multiple times in
      // a row. We only care about the case when ID for the diff is provided
      // because that's what we need to apply the diff and trigger the tests.
      if (strlen($diffID) > 0) {
        assert(is_numeric($diffID));
        startTestsInSandcastle(true /* $applyDiff */, $workflow, $diffID);
      }
    }
  }
}
