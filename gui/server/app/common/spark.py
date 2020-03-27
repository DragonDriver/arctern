"""
Copyright (C) 2019-2020 Zilliz. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

from pyspark.sql import SparkSession

from app.common import config
from arctern_pyspark import register_funcs
import os
x = '/home/ljq/ljq_hadoop_conf_dir/hadoop'
os.environ['JAVA_HOME'] = '/usr/lib/jvm/java-8-openjdk-amd64'
os.environ['HADOOP_CONF_DIR'] = x
os.environ['YARN_CONF_DIR'] = x
os.environ['PYSPARK_PYTHON'] = "/home/spark/miniconda3/envs/arctern/bin/python"
os.environ['PYSPARK_DRIVER_PYTHON'] = "/home/ljq/miniconda3/envs/arctern/bin/python"



class Spark:
    """
    the singleton of this class keeps the session of spark
    """

    def __init__(self):
        try:
            print("DDDDDDDDDDDDDD", os.environ['JAVA_HOME'])
        except:
            pass

        self.session = SparkSession.builder \
            .appName("Arctern") \
            .master(config.INSTANCE.get("spark", "master-addr")) \
            .config("yarn.resourcemanager.address", config.INSTANCE.get("yarn", "resource-manager-address")) \
            .config("spark.executorEnv.PROJ_LIB", "/home/spark/miniconda3/envs/arctern/share/proj") \
            .config("spark.executorEnv.PYSPARK_PYTHON",
                    config.INSTANCE.get("spark", "executor-python")
                    ) \
            .config("spark.sql.execution.arrow.pyspark.enabled", "true") \
            .config("spark.databricks.session.share", "false") \
            .getOrCreate()
        register_funcs(self.session)

    def create_session(self):
        """
        clone new session
        """
        return self.session.newSession()

    @staticmethod
    def run(sql):
        """
        submit sql to spark
        """
        session = INSTANCE.create_session()
        register_funcs(session)
        return session.sql(sql)

    @staticmethod
    def run_for_json(sql):
        """
        convert the result of run() to json
        """
        _df = Spark.run(sql)
        return _df.coalesce(1).toJSON().collect()


INSTANCE = Spark()
