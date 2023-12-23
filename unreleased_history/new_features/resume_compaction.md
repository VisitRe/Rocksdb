Provide an experimental option `Options::resume_compaction` to resume unfinished compactions left from the last db session. Right now only unfinished remote compactions due to primary db restart or failed remote compaction are supported. This options is turned on by default and has no effect to users with no remote compaction (i.e, `Options::compaction_service == nullptr`) or disable auto compaction (i.e, `Options::disable_auto_compactions = true`)