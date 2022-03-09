#!/usr/bin/env python3
#  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).

'''Access CircleCI output via a combination of v1 and v2 APIs
In order to finally yield the output of benchmark tests.
'''

import sys
from typing import Callable, List
import circleci.api
import requests


def api_v2_find_job() -> int:
    '''Track down the job
    TODO (AP) see insomnia logs for the path to the latest (or multiple) pipelines / workflows / benchmarks
    '''
    # Start by listing the pipelines
    # https://circleci.com/api/v2/project/github/facebook/rocksdb/pipeline
    # We get back a list of items
    # We have selected the item with "id": "e9841441-5892-4ddb-8aa8-feb9696ce923"
    # trigger.actor.login == "alanpaxton" && vcs.branch == "pull/9676"
    # to get the workflows https://circleci.com/api/v2/pipeline/e9841441-5892-4ddb-8aa8-feb9696ce923/workflow
    # This contains items, one of which has "name": "benchmark-linux"
    # We use the id of that workflow item "id": "bcd4d608-a777-4db0-9d6e-5d54c2bb79d0"
    # to get job number https://circleci.com/api/v2/workflow/bcd4d608-a777-4db0-9d6e-5d54c2bb79d0/job


NEXT_CALL_RANGE = 5


class CircleAPIV2:

    def __init__(self, user_id: str, vcs: str, username: str, project: str) -> None:
        '''Configure with a CircleCI user id, and a slug (roughly, the project, e.g. github/facebook/rocksdb)
            '''
        self.auth = (user_id, '')
        self.slug = f"{vcs}/{username}/{project}"
        self.service = "https://circleci.com/api/v2"

    def get_jobs(self) -> List[int]:
        '''TODO AP
        '''
        pass

    def get_workflow_items(self, pipeline_id: str, filter: Callable) -> List[str]:
        '''All the workflow items
        TODO AP filter to where item.name == "benchmark-linux"
        '''
        params = {}
        result = []
        for i in range(NEXT_CALL_RANGE):
            workflows = requests.get(
                f"{self.service}/pipeline/{pipeline_id}/workflow", auth=self.auth, params=params)
            workflows.raise_for_status()
            json = workflows.json()
            result = result + [item['id']
                               for item in json['items'] if filter(item)]
            if json['next_page_token'] == None:
                break
            params = {'page-token': json['next_page_token']}
        return result

    def get_pipeline_ids(self, filter: Callable) -> List[str]:
        params = {}
        result = []
        for i in range(NEXT_CALL_RANGE):
            pipelines = requests.get(
                f"{self.service}/project/{self.slug}/pipeline", auth=self.auth, params=params)
            pipelines.raise_for_status()
            json = pipelines.json()
            result = result + [item['id']
                               for item in json['items'] if filter(item)]
            if json['next_page_token'] == None:
                break
            params = {'page-token': json['next_page_token']}

        return result

    def get_jobs(self, workflow_id: str) -> List[int]:
        params = {}
        result = []
        for i in range(NEXT_CALL_RANGE):
            jobs = requests.get(
                f"{self.service}/workflow/{workflow_id}/job", auth=self.auth, params=params)
            jobs.raise_for_status()
            json = jobs.json()
            result = result + [item['job_number']
                               for item in json['items']]
            if json['next_page_token'] == None:
                break
            params = {'page-token': json['next_page_token']}

        return result


def is_my_pull_request(pipeline):
    '''TODO AP'''
    try:
        return pipeline['vcs']['branch'] == "pull/9676"
    except KeyError:
        return False


def is_benchmark_linux(step):
    return step['name'] == "benchmark-linux"


def always_true(x):
    return True


class CircleAPIV1:
    def __init__(self, user_id: str, vcs: str, username: str, project: str) -> None:
        '''Configure with a CircleCI user id, and the vcs, username, project, e.g. github, facebook, rocksdb
        '''
        self.api = circleci.api.Api(user_id)
        self.vcs = vcs
        self.username = username
        self.project = project

    def get_log_action_output_url(self, job_number: int, action_name: str) -> str:
        '''Get the URL of the output of an action.
        '''

        dict = self.api.get_build_info(username=self.username,
                                       project=self.project, build_num=job_number)
        for step in dict['steps']:
            for action in step['actions']:
                if action['has_output'] and action['name'] == action_name:
                    # found the special action whose content we want
                    return action['output_url']
        return None

    def get_log_mime_url(self, job_number: int) -> str:
        '''Use the old CircleCI v1.1 API to get the report URL. The report is the output files, MIME encoded.
        Track it down because we know its name.
        '''

        return self.get_log_action_output_url(job_number, "Output logs as MIME")


def flatten(ll):
    return [item for l in ll for item in l]


def main():
    # track down the job number
    api_v1 = CircleAPIV1(user_id="e7d4aab13e143360f95e258be0a89b5c8e256773",
                         vcs="github", username="facebook", project="rocksdb")
    api_v2 = CircleAPIV2(user_id="e7d4aab13e143360f95e258be0a89b5c8e256773",
                         vcs="github", username="facebook", project="rocksdb")
    #job_number = api_v2_find_job()
    # job number can be got from the v2 API
    # api_v1_get_log_mime_url(job_number=317985)
    log_mime_url = api_v1.get_log_mime_url(job_number=317985)
    pipeline_ids = api_v2.get_pipeline_ids(filter=is_my_pull_request)
    workflows = flatten([api_v2.get_workflow_items(
        pipeline_id, filter=is_benchmark_linux) for pipeline_id in pipeline_ids])
    jobs = flatten([api_v2.get_jobs(workflow_id) for workflow_id in workflows])
    urls = [api_v1.get_log_mime_url(job_number=job_id) for job_id in jobs]

    return 0


if __name__ == '__main__':
    sys.exit(main())
