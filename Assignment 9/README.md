## Assignment 9: Query Plans 1 - in PostgreSQL

### **Query Evaluation Plans**

#### Part 1: Loading data

***(This part does not require a submission)***

1. Load the university schema [from this file](resources/DDL.sql) and populate it with a large dataset [from this file](resources/largeRelationsInsertFile.sql). If you need to drop tables, [use this file](resources/DDL-drop.sql).

   1. **Do not use pgadmin3/pgadmin4 for loading data as it may hang.**

   2. open terminal and start `psql` with appropriate port number and database name (refer 1^st^ lab on PostgreSQL setup)

   3. From within `psql`, run the command `\i <filename>` *e.g.* just type `\i DDL.sql` OR `\i largeRelationsInsertFile.sql`. Run the command from the same directory where you saved the file.

   5. You will see `INSERT 0 1` being printed for every insert. It will take some time but will terminate successfully. You may see an unchanging screen even though the system is continuously printing the above line. Hit enter to see if prints are ongoing.

#### Part 2: Studying query plans

***(This part does not require a submission)***

**NOTE**: *Execute the queries for this part using pgadmin3, since you get a much better looking output, and you can even see query trees in the Explain tab.*

1. Before going further, run the `ANALYZE` command:
   - `ANALYZE` collects statistics about the contents of tables in the database, and stores the results in the system catalog. Subsequently, the query planner uses these statistics to help determine the most efficient execution plans for queries.
   - With no parameter, `ANALYZE` examines every table in the current database. With a parameter, `ANALYZE` examines only that table. It is further possible to give a list of column names, in which case only the statistics for those columns are collected.

1. **NOTE**: PostgreSQL uses a technique called bitmap-index-scan, which is used for secondary index access to multiple tuples. It scans the secondary index, and creates a bitmap with 1 bit per page in the file, and sets the page bit to 1 if the index shows a relevant tuple is in that page. After finishing the index scan the bitmap is complete, and pages are now scanned in physical order (using the bitmap heap scan operator) skipping pages whose bit is 0. For each retrieved page, all tuples are scanned, and the original selection condition has to be rechecked, since the page may contain tuples that did not satisfy the original condition (as few as 1 tuple may satisfy the condition). Bitmap index scans are useful only when multiple tuples are retrieved using a secondary index.

2. Run each of the following queries to find the time taken, and use the explain feature to find the plan used for each of the queries. By studying the plans, try to figure out why each query either ran fast or ran slowly. No need to submit anything for this part of the assignment, just see what plans are generated).

   ```sql
   select * from student where ID = '1000'
   select * from section where course_id = 'CS-101'
   select * from takes natural join student;
   select * from takes natural join student where ID = '1000';
   select * from student, instructor where student.id = instructor.id and student.id = '1000'
   select * from student, instructor where upper(student.id) = upper(instructor.id) and student.id = '1000'
   ```

3. Learn about `pg_stats`, by reading the manual ([ https://www.postgresql.org/docs/10/static/view-pg-stats.html](https://www.postgresql.org/docs/9.6/static/view-pg-stats.html)). Then run the following queries to see some of the statistics and other catalog information
   
```sqlite
   select * from pg_stats where tablename = 'student'
   select * from pg_indexes where tablename not like 'pg_%'
```

#### Part 3: Queries

***(This part REQUIRES a submission)***

You can submit either a text file, or a .odt/.docx/.pdf file. For each case, submit

- a query
- the chosen plan
- your reasoning behind constructing the query (briefly)

1. Create a selection query whose chosen plan is a file scan.
2. Create a selection query whose chosen plan uses a bit-map index scan.  You can create indices on appropriate relation attributes to create such a case.
3. Create a selection query whose chosen plan is an index scan followed by a filter operation (NOTE: in the plan printed by PostgreSQL, the filter is shown below the index condition, but it is actually executed after the index lookup.)
4. Create a query where PostgreSQL chooses a (plain) index nested loops join (NOTE: the nested loops operator has 2 children. The first child is the outer input, and it may have an index scan or anything else, that is irrelevant. The second child must have an index scan or bitmap index scan, using an attribute from the first child.)
5. Create a query where PostgreSQL chooses a merge join (hint: use an order by clause)
6. Add a LIMIT 10 ROWS clause at the end of the previous query, and see what algorithm method is used.  (LIMIT *n* ensures only *n* rows are output.) Explain what happened, if the join algorithm changes; if the plan does not change, create a different query where the join algorithm changes as a result of adding the LIMIT clause.
7. Create an index as below, and see the time taken: `create index i1 on takes(id, semester, year);`
      Similarly see how long it takes to drop the above index using: `drop index i1;`
8. PostgreSQL does not create indexes on foreign key columns by default. See the effect of having/not having such an index by measuring the execution time of a deletion query. To do so execute the following:
   1. `begin;`. This is important since you want to roll back the deletion later, to avoid reloading the database!

   2. Now run
      
      ```sql
      explain analyze delete from course where course_id = '400';
      ```
      
      and find the time it takes.  Record this time.
      *Note that PostgreSQL does not explain how it checks for foreign key constraint violation.*
      
   3. Finally run `rollback;` to restore the database state

   4. Next add indices on all foreign keys; for our purpose these will do: (a) all foreign keys that reference course_id directly and (b) on foreign keys that reference these relations (transitively!). For your convenience, these are listed below.

      <pre>
      section (course_id), prereq (course_id), prereq(prereq_id)
      teaches(course_id, sec_id, semester, year)
      takes(course_id, sec_id, semester, year)
      </pre>

   5. And now measure the time it takes for the same delete as above, using the same 3 steps you used earlier.

   6. Submit the timings along with an explanation of the results you observe.