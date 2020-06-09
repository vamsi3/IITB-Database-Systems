import csv
import re
import sys
import subprocess
import psycopg2
import json
from pg_query import Node, parse_sql
import math
import threading
import datetime
import time
import itertools


#### PARSING QUERY PLAN ##################################################
########################################################################

def findTablesCols(plan,reln):
	tables = {}
	columns = []
	gp_columns = []
	for condition in ["Group Key", "Sort Key"]:
		if condition in plan:
			temp = plan[condition]
			for t in temp:
				t = t.split(" ")
				t = t[0]
				if t.find(".") != -1:
					columns.append(t.split("."))
				else:
					columns.append([reln,t])

	if "Relation Name" in plan:
		reln = plan["Relation Name"]
		if "Alias" in plan:
			tables[plan["Alias"]] = plan["Relation Name"]
			tables[plan["Relation Name"]] = plan["Relation Name"]
		else:
			tables[plan["Relation Name"]] = plan["Relation Name"]
	
	for condition in ["Hash Cond", "Index Cond", "Filter", "Join Filter"]:
		if condition in plan:
			cond = plan[condition]


			i2 = 0
			while i2 < len(cond)-2:
				cond1 = cond[i2:]
				atomic_conds = []
				curr_pos = cond1.find(" AND ")
				#print(cond1)
				if curr_pos == -1  :
					i2 += 1
					continue

				[left, right, left_depth, right_depth] = [curr_pos,curr_pos+4,0,0]

				while left_depth <= 0:
					left -= 1
					if left < 0 :
						continue
					if cond1[left] == "(":
						left_depth += 1
					if cond1[left] == ")":
						left_depth -= 1

				while right_depth >= 0 :
					right += 1
					if right >= len(cond1) :
						continue
					if cond1[right] == "(":
						right_depth += 1
					if cond1[right] == ")":
						right_depth -= 1


				i2 = right

				and_expr = cond1[left+1:right]
				atomic_conds = and_expr.split(" AND ")
				#print(and_expr)
				and_table_cols = []
				for atomic_cond in atomic_conds :
					#print(atomic_cond)
					for i1 in range(0,len(atomic_cond)-2):
						if atomic_cond[i1] == "(" and atomic_cond[i1+1] != "(" and atomic_cond[i1+1] != "'" and atomic_cond[i1+1] != "$":
							j1 = i1
							while atomic_cond[j1] != ")" and atomic_cond[j1:j1+2] != " =" and atomic_cond[j1] != " ":
								#print(atomic_cond[j:j+1])
								j1 += 1
							
							temp = atomic_cond[i1+1:j1]
							if temp.find(".") != -1:
								and_table_cols.append(temp.split("."))
							else:
								and_table_cols.append([reln,temp])

							i1 = j1
						elif atomic_cond[i1:i1+2] == "= " and atomic_cond[i1+2] != "(" and atomic_cond[i1+2] != "'" and atomic_cond[i1+2] != "$":
							j1 = i1+2
							while atomic_cond[j1] != ")" and atomic_cond[j1] != " ":
								j1 += 1
							
							temp = atomic_cond[i1+2:j1]
							if temp.find(".") != -1:
								and_table_cols.append(temp.split("."))
							else:
								and_table_cols.append([reln,temp])
							i1 = j1
				table_dict = {}
				for el in and_table_cols :
					if el[0] in table_dict :
						table_dict[el[0]].append(el[1])
					else :
						table_dict[el[0]] = [el[1]]
				#print(table_dict)
				for table_name in table_dict :
					temp = [table_name,table_dict[table_name]]
					gp_columns.append(temp)
			for i in range(0,len(cond)-2):

				if cond[i] == "(" and cond[i+1] != "(" and cond[i+1] != "'" and cond[i+1] != "$":
					j = i
					while cond[j] != ")" and cond[j:j+2] != " =" and cond[j] != " ":
						#print(cond[j:j+1])
						j += 1
					
					temp = cond[i+1:j]
					if temp.find(".") != -1:
						columns.append(temp.split("."))
					else:
						columns.append([reln,temp])

					i = j
				elif cond[i:i+2] == "= " and cond[i+2] != "(" and cond[i+2] != "'" and cond[i+2] != "$":
					j = i+2
					while cond[j] != ")" and cond[j] != " ":
						j += 1
					
					temp = cond[i+2:j]
					if temp.find(".") != -1:
						columns.append(temp.split("."))
					else:
						columns.append([reln,temp])

					i = j


	if "Plans" in plan:
		plans = plan["Plans"]
		#print(plans)
		for p in plans:
			[part_t,part_c,part_gc] = findTablesCols(p,reln)
			temp_tables = tables.copy()
			temp_tables.update(part_t)
			tables = temp_tables
			columns = columns + part_c
			gp_columns = gp_columns + part_gc

	return [tables,columns,gp_columns]

##############################################################
################################################################

# Open Connection to database
conn = psycopg2.connect(database = "test", user = "praneeth", password = "", host = "127.0.0.1", port = "6001")

def getTimeFactor(ts1,ts2,sig):
	timeDiff = ts2 - ts1
	return math.exp(-timeDiff/sig)

drop_log = {}
lock = threading.Lock()
aggr_time = 0
indexed_cols ={}

def drop_index(thresold):
	while 1:
		index_names = []
		lock.acquire()
		index_wt = {}

		for fp in drop_log :
			index_name = drop_log[fp][0]
			base_wt = drop_log[fp][1]
			timestamp1 = drop_log[fp][2]
			timestamp2 = int(time.time())

			drop_log[fp][2] = timestamp2
			drop_log[fp][1] = base_wt*getTimeFactor(timestamp1,timestamp2,1)

			if index_name in index_wt :
				index_wt[index_name][0] += base_wt
			else :
				index_wt[index_name] = [base_wt,timestamp1] 

		for index_name in index_wt :
			base_wt = index_wt[index_name][0]
			timestamp1 = index_wt[index_name][1]		
			#print("base weight "+index_name+" "+str(base_wt))

			if(base_wt < thresold):
				cur = conn.cursor()
				cur.execute("drop index "+index_name) 
				conn.commit()
				print("DROP INDEX "+index_name)

				index_names.append(index_name)
		#print()
		rem_list = []
		for name in index_names :
			for fp in drop_log :
				if drop_log[fp][0] == name :
					rem_list.append(fp)
		for fp in rem_list :
			if fp in drop_log :
				del drop_log[fp]
		lock.release()
		time.sleep(5)

def hypo_index(query,cost_before,tables,columns,raw_gp_columns):
	
	#print("Opened database successfully")
	cur = conn.cursor()

	# Enable hypopg extension in postgresql
	# enable_ext = "CREATE EXTENSION hypopg"
	# cur.execute(enable_ext)

	stmt_get_plan = "explain (format json) " + query

	# Parsing :

	table_col = []
	gp_columns = []

	for el in columns :
		if el[0] == '' or el[1] == '' : 
			continue
		stmt_check_table = "select exists ( select 1 from information_schema.tables where table_name = '"+tables[el[0]]+"' and table_schema not in ('pg_catalog','information_schema') );"
		cur.execute(stmt_check_table)
		result = str(cur.fetchall())
		#print(result)
		if result.find("True") != -1 :
			table_col.append([tables[el[0]],el[1]])

	for el in raw_gp_columns :
		if el[0] == '' or el[1] == '' : 
			continue	
		stmt_check_table = "select exists ( select 1 from information_schema.tables where table_name = '"+tables[el[0]]+"' and table_schema not in ('pg_catalog','information_schema') );"
		cur.execute(stmt_check_table)
		result = str(cur.fetchall())
		#print(result)
		if result.find("True") != -1 :
			gp_columns.append([tables[el[0]],el[1]])

	#print(table_col)
	#print(gp_columns)

	table_all_cols = {}
	for el in table_col :
		if el[0] in table_all_cols :
			table_all_cols[el[0]].append(el[1]);
		else :
			table_all_cols[el[0]] = [el[1]];

	# table, column to TEST INDEX on; A frequent query using this column
	fin_table = ""
	fin_col = []

	maxDiff = 0
	# Create hypothetical index
	if table_col != [] :
		#print(table_all_cols)
		for table in table_all_cols : 
			all_cols = table_all_cols[table]
			for col in all_cols :
				stmt_hypo_index = "select * from hypopg_create_index('create index on "+table+"("+col+") ')"
				cur.execute(stmt_hypo_index)
				result = cur.fetchall()
				hypo_index_id = str(result[0][0])

				#Estimate the execution Cost after indexing
				cur.execute(stmt_get_plan)
				result = cur.fetchall()
				cost_after = float(result[0][0][0]["Plan"]["Total Cost"])
				#print("DEBUG : "+str(cost_before)+" "+str(cost_after))

				# Remove hypothetical index
				stmt_hypo_remove = "select * from hypopg_drop_index("+hypo_index_id+")"
				cur.execute(stmt_hypo_remove);

				if cost_before - cost_after > maxDiff :
					maxDiff = cost_before - cost_after
					fin_table = table
					fin_col = [col]
			for col1 in all_cols:
				for col2 in all_cols:
					if col1 == col2 :
						continue

					stmt_hypo_index = "select * from hypopg_create_index('create index on "+table+"("+col1+","+col2+") ')"
					cur.execute(stmt_hypo_index)
					result = cur.fetchall()
					hypo_index_id = str(result[0][0])

					#Estimate the execution Cost after indexing
					cur.execute(stmt_get_plan)
					result = cur.fetchall()
					cost_after = float(result[0][0][0]["Plan"]["Total Cost"])
					#print("DEBUG : "+str(cost_before)+" "+str(cost_after))

					# Remove hypothetical index
					stmt_hypo_remove = "select * from hypopg_drop_index("+hypo_index_id+")"
					cur.execute(stmt_hypo_remove);

					if cost_before - cost_after > maxDiff :
						maxDiff = cost_before - cost_after
						fin_table = table
						fin_col = [col1,col2]

		for el in gp_columns :
			table = el[0]
			ordered_cols = el[1]
			all_perms = list(itertools.permutations(ordered_cols))

			for conj_cols in all_perms : 
				stmt_hypo_index = "select * from hypopg_create_index('create index on "+table+"("+",".join(conj_cols)+") ')"
				cur.execute(stmt_hypo_index)
				result = cur.fetchall()
				hypo_index_id = str(result[0][0])

				#Estimate the execution Cost after indexing
				cur.execute(stmt_get_plan)
				result = cur.fetchall()
				cost_after = float(result[0][0][0]["Plan"]["Total Cost"])
				#print("DEBUG : "+str(cost_before)+" "+str(cost_after))

				# Remove hypothetical index
				stmt_hypo_remove = "select * from hypopg_drop_index("+hypo_index_id+")"
				cur.execute(stmt_hypo_remove);

				if cost_before - cost_after > maxDiff :
					maxDiff = cost_before - cost_after
					fin_table = table
					fin_col = conj_cols

		# if maxDiff > 0 :
		# 	print("Total Cost before indexing : "+str(cost_before))
		# 	print("Total Cost after indexing : "+str(cost_before-maxDiff))
		#	print("Potential index on : {0}::{1}".format(fin_table,fin_col))
		# if maxDiff == 0:
			# print("Total Cost before indexing : "+str(cost_before))
			# print("Total Cost after indexing : "+str(cost_before-maxDiff))
			#print("No potential index found")
			#print()
			#print("++"*20)
			#print()
	# else :
	# 	print()
	# 	print("**"*20)
	# 	print()

	conn.commit()

	return [fin_table,fin_col,maxDiff]


# Main Code ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
table_numrows = {}
drop_enable = 1
log = {}
features = {}
loglocation = "./log/db_project_log.csv"
[aggr_time1,aggr_time2,start_time,end_time] = [0,0,0,0]
counter = 0

if drop_enable == 1:
	thread1 = threading.Thread(target=drop_index,args=[10])
	thread1.start()
reader = csv.reader(iter(sys.stdin.readline, ''))

for row in reader:
	rowlength = len(row)
	fingerprint = row[rowlength-4]
	timestamp = eval(row[rowlength-3])
	query = re.sub(' +',' ',row[rowlength-2].replace('\n', " "))
	cost = float(row[rowlength-1])

	#print(query)

	#### Acuisition of query info over #########################################################
	#### Now compute indexing cost and diff in cost due to indexing ####################################

	################################### AUTOMATIC INDEXING ###############################################

	change = 0
	[fin_table,fin_col,maxDiff] = [0,0,0]

	if fingerprint in log :

		prev_entry = log[fingerprint]
		prev_features = features[fingerprint]
		if abs(cost - prev_entry[2]) > cost :
			change = 1
			start_time = time.time()

			[fin_table,fin_col,maxDiff] = hypo_index(query,cost,prev_features[0],prev_features[1],prev_features[2])

			end_time = time.time()
			aggr_time1 += end_time - start_time
			#print("Cost Change "+query+" "+prev_entry[2]+" "+str(cost))

			if fin_table == "" or fin_col == "":
				continue
			new_aggr = prev_entry[1]*getTimeFactor(timestamp,prev_entry[6],100) + maxDiff
			log[fingerprint] = [query,new_aggr,cost,fin_table,fin_col,maxDiff,timestamp] 
		else :
			[fin_table,fin_col,maxDiff] = [prev_entry[3],prev_entry[4],prev_entry[5]]
			new_aggr = prev_entry[1]*getTimeFactor(timestamp,prev_entry[6],100) + maxDiff
			log[fingerprint] = [query,new_aggr,cost,prev_entry[3],prev_entry[4],prev_entry[5],timestamp]
		lock.acquire()
		if fingerprint in drop_log : 
			drop_log[fingerprint][1] += 1 
		lock.release()
		
	else :
		cur = conn.cursor()
		stmt_get_plan = "explain (format json) " + query
		cur.execute(stmt_get_plan)
		result = cur.fetchall()
		plan = result[0][0][0]["Plan"]

		conn.commit()

		[tables,columns,raw_gp_columns] = findTablesCols(plan,"")
	
		change = 1
		[fin_table,fin_col,maxDiff] = hypo_index(query,cost,tables,columns,raw_gp_columns)
		if fin_table == "" or fin_col == "":
			continue
		log[fingerprint] = [query,maxDiff,cost,fin_table,fin_col,maxDiff,timestamp]	
		features[fingerprint] = [tables,columns,raw_gp_columns]

	# print(fin_table)
	# print(fin_col)
	if maxDiff > 0 :  
		# Number of rows for indexing cost estimate
		num_rows = 0
		if fin_table in table_numrows and change == 0:
			num_rows = table_numrows[fin_table]
		else :
			cur = conn.cursor()
			stmt_get_numrows = "select reltuples from pg_class where relname = '"+fin_table+"'; "

			start_time = time.time() 

			cur.execute(stmt_get_numrows)

			end_time = time.time()
			aggr_time2 += end_time - start_time

			result = cur.fetchall()
			conn.commit()

			num_rows = result[0][0]
			table_numrows[fin_table] = num_rows
	
		#print("Number of rows in table "+fin_table+" "+str(num_rows))
		indexing_cost = 0.1*num_rows #### REPLACE WITH FUNCTION OF num_rows !!!!!!!!!!!!!!!!!!!!!!!

		### DO Ski-Rental to know if Indexing is useful & Create index ##########
		if log[fingerprint][1] > indexing_cost :

			cur = conn.cursor()
			index_name = "i_"+fin_table+"_"+"_".join(fin_col)
			stmt_create_index = "create index if not exists "+index_name+" on "+fin_table+"(" + ",".join(fin_col) + ");"
			lock.acquire()
			if index_name not in indexed_cols :  
				print("Creating index on "+fin_table+"::"+",".join(fin_col)+" ctr "+str(counter))
				drop_log[fingerprint] = [index_name,log[fingerprint][1]/maxDiff,timestamp];
				indexed_cols[index_name] = 1
				cur.execute(stmt_create_index)
			lock.release()
			conn.commit()
			log[fingerprint][1] = 0;
	counter += 1
	# if(counter % 200 == 0) :
	# 	print("DEBUG: "+query+" "+str(indexing_cost)+" "+str(log[fingerprint][1]))

	if(counter % 1000 == 0) :
		print(str(aggr_time1)+" "+str(aggr_time2))

thread1.join()