import csv
import re
import sys
import subprocess
import psycopg2
import json
import math
import random
import time

# Open Connection to database
conn = psycopg2.connect(database = "test", user = "postgres", password = "", host = "localhost", port = "5432")

cur = conn.cursor()

tot_tests=100

get_ins_id = "select id from instructor;"
get_deps = "select dept_name from department;"
get_sname="select distinct name from student limit 50;"
cur.execute(get_ins_id)
iids = cur.fetchall()
iids = [x[0] for x in iids]
cur.execute(get_deps)
deps= cur.fetchall()
deps=[x[0] for x in deps]
cur.execute(get_sname)
snames=cur.fetchall()
snames=[x[0] for x in snames]

conn.commit()
conn.close()

stmt1 = "select * from advisor  where i_id='{}';"
stmt21 = "select * from student where tot_cred={}"
stmt22 = "select * from student where name<='{}'"

stmt3 = "select * from student where dept_name = '{}' and tot_cred>={}"

start_time=time.time()
for i in range(tot_tests):
	conn = psycopg2.connect(database = "test", user = "praneeth", password = "", host = "127.0.0.1", port = "6001")
	cur = conn.cursor()
	if i%100 == 0:
		print("Iteration {}".format(i))
	cur.execute(stmt1.format(iids[random.randint(0,len(iids)-1)]))
	result = str(cur.fetchall())

	cur.execute(stmt21.format(random.randint(0,129)))
	result = str(cur.fetchall())

	cur.execute(stmt22.format(snames[random.randint(0,len(snames)-1)]))
	result = str(cur.fetchall())
	
	cur.execute(stmt3.format(deps[random.randint(0,len(deps)-1)] , random.randint(0,129)))
	result = str(cur.fetchall())
	conn.commit()
	conn.close()

time_taken=time.time()-start_time
print("Time taken {}".format(time_taken))

