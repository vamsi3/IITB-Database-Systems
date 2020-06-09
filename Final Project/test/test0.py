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

tot_tests=100000

get_ins_id = "select id from instructor;"
cur.execute(get_ins_id)
ids = cur.fetchall()
ids = [x[0] for x in ids]

stmt1 = "select * from advisor  where i_id='{}';"

start_time = time.time()

for i in range(tot_tests):
	if i%10000 == 0:
		print("Iteration {}".format(i))
	cur.execute(stmt1.format(ids[random.randint(0,len(ids)-1)]))
	result = str(cur.fetchall())
	#time.sleep(0.001)

end_time = time.time()

print("Time taken: {}".format(end_time - start_time))

conn.close()	