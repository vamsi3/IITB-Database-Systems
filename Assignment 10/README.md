## Assignment 10: Query Plans 2 + Transactions - in PostgreSQL

_**Part 1: This part does not involve a submission:** It is for your general knowledge, and can be done after the submission part._

1. Read the overview of PostgreSQL internals here to understand how a query is processed: https://www.postgresql.org/docs/10/static/overview.html
   This is a very brief summary, so you can read all its subsections in just about 15 minutes.
2. Read how PostgreSQL estimates number of rows in a select or join result here:
   [ https://www.postgresql.org/docs/10/static/row-estimation-examples.html](https://www.postgresql.org/docs/10/static/row-estimation-examples.html)
3. You can modify and try out some of the queries in the above page, replacing tenk1 and tenk2 by takes, and attribute names or tenk1 and tenk2 by attribute names from takes such as id, and course_id.
   This part should not take more than 30 minutes.



_**Part 2: This part needs submission:** For each question, submit a query along with the plan chosen by PostgreSQL.  You can submit either a text file, or a a .odt/.docx/.pdf file containing both the query and the plan. Where required **use explain analyze to get the actual execution costs** (specified in the question)._

**NOTE:** This assignment needs a large database (which you have already loaded as part of the previous assignment). Refer to instructions for Assignment 9: Part 1 for loading the large database. Alternatively, the required SQL files can be found in `resources/` directory.

1. Consider the following nested subquery with an exists clause:
   
   ```sql
select count(\*) from course c where exists (select \* from takes t where t.course_id = c.course_id)
   ```
   
   What is the plan is chosen by PostgreSQL. Report the plan and actual execution costs. Explain what is happening.
   
2. As above, but with a not exists clause.

3. PostgreSQL is often unable to (or decides not to) decorrelate complex subqueries. Write a correlated subquery which computes an aggregate, which is used in a predicate in the outer query, which is executed using correlated evaluation (shown as a subplan in the explain result).  *<u>Use a large relation such as takes in the subquery</u>*. Report the query and the plan.

4. Now manually rewrite your query from Q3 to perform decorrelation (*i.e.* write an equivalent query that does not use subqueries but uses joins instead; make sure duplicate counts are not affected).
   *NOTE: Use a WITH clause, instead of create table as, to avoid the overheads of creation of a table when not really required.*
   Show the query and the actual execution plan. Report the difference in actual execution time of the correlated and decorrelated queries.

5. Create a materialized view using from the query:

     ```sql
     select c.course_id, count(ID) from course c, takes t where t.course_id = c.course_id group by c.course_id
     ```

     - Note that PostgreSQL does not currently support materialized view maintenance by any technique other than full refresh, and requires maintenance to be done by a REFRESH MATERIALIZED VIEW command. However, other databases do support automatic incremental maintenance of materialized views.

6. Now run an `explain analyze select count(*) from` on the materialized view relation, as well as on the query defining the materialized view. What were the estimated cost and actual time in the two cases?

      - This should show you why materialized views are useful. It should also show you that the PostgreSQL optimizer is not able to automatically rewrite queries to use the materialized view.

7. Now create a materialized view defined by the query

   ```sql
   select * from takes natural join student
   ```


   Run explain analyze on a query that selects count(*) from the materialized view, where id = '14182'. Repeat this using the definition of the materialized view instead of directly using the view. Give the plans and explain what you observed. What should the optimizer have ideally done in this case?

8. *This question requires PostgreSQL 9.6 or later; if you have an older version, upgrade, or try this on a lab machine which has PostgreSQL 10.*
      Now let's try to get PostgreSQL to use parallel query plans. To do this we need to do the following

      1. Execute the command:  set max_parallel_workers_per_gather=4;
            - Instead of 4, you can use any value > 1.
      2. Create a large table. Using the large relations from last lab, takes has 30,000 rows which is too small for parallel querying. Create a larger relation as follows:
            1. `create table bigtakes as select * from takes;`
            2. `insert into bigtakes select * from bigtakes;`
            3. repeat the above step several times to double the size of bigtakes, to take bigtakes to 480000 rows.
      3. Now run `explain analyze select count(*) from bigtakes;`
         Give the result, and explain what happened in the plan.
         To understand more about parallel query processing in PostgresQL, read: https://www.postgresql.org/docs/current/static/how-parallel-query-works.html

9. **Transactions and Concurrency

      PostgreSQL's default concurrency control level, is called *read committed,* *where reads are done from a snapshot* (not all databases support read committed this way, but PostgreSQL does), writes are performed at the end of the transaction. The concurrency level can be changed as described later in this question.

      1. In this exercise you will run transactions concurrently from two different `pgAdmin`/`psql` windows, to see how updates by one transaction affect another.

         - Open two pgAdmin or psql connections to the same database. Execute the following commands in sequence in the first window

           ```sql
           begin;
           set transaction isolation level serializable;
           select tot_cred from student where name = 'Keiss';
           --Note the value of tot_cred above.
           update student set tot_cred = 55 where name = 'Keiss';
           ```

         - Now in the second window execute

           ```sql
           begin;
           set transaction isolation level serializable;
           select tot_cred from student where name = 'Keiss';
           ```
           
           Look at the value of tot_cred.  Explain how this happened, in terms of the concurrency control mechanism used by PostgreSQL.
         
         - Now in the first window execute
         
           ```sql
           commit;
           ```
         
         - And in the second window execute
         
           ```sql
           select tot_cred from student where name = 'Keiss';
           commit;
           ```
           
           Observe how although the first transaction has already committed the second transaction got the old value for tot_cred.  Explain why this happened.

      2. Open two connections (two new query windows) and type the following:

         1. Run the following query  and note the results:

            ```sql
            select id, salary from instructor where id in('63395', '78699')
            ```

         2. In both windows: Begin a transaction using `begin;`

         3. In both windows: Execute the command:

            ```sql
            set transaction isolation level serializable;
            ```

         4. Run this query in window 1:

            ```sql
            update instructor set salary = (select salary from instructor where id = '63395') where id = '78699';
            ```

         5. Run this query in window 2:

            ```sql
            update instructor set salary = (select salary from instructor where id = '78699') where id = '63395';
            ```

         6. Execute `commit;` in window 1

         7. Execute `commit;` in window 2

         8. What happened above?  Check the state of the system by running the query

            ```sql
            select id, salary from instructor where id in('63395', '78699')
            ```

         9. NOTE: Here, PostgreSQL uses a concurrency control technique called *serializable snapshot isolation, where each transaction sees a snapshot of the database as of when it starts. If both transactions were allowed to commit in this case, you would have a final state which cannot be obtained by executing the transactions one after another in either order.*

      3. Redo Part 2 above, but with isolation level set to read committed. What difference did you observe, and what does it say about serializability with the read committed isolation level?

