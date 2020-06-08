## Assignment 4A: JDBC (In-Class)

### Part 1: Related updates

Write a Java program using JDBC to update the grade of a student as follows: take as input from the terminal/console `ID course_id sec_id semester year grade` (separated by spaces) and update the grade of an existing tuple in the takes relation; but also, depending on change from pass to fail **or vice-versa,** update the students `tot_cred` correctly. 

- NOTE 1: null and F are fail grades, all other grades are pass grades; grade values provided by the user above can be assumed to be valid grades, and you can assume the user will not type null as a grade. 

- NOTE 2: We assume for simplicity that a student who has already passed the course will not be allowed to take it again.)

- NOTE 3: You can assume that tot_cred is not null;


Both updates should be done as part of the same transaction by turning off auto commit, and issuing a `connection.commit()` at the end. (In case there is no matching takes tuple, output a corresponding message; in your assignment you are checking for old value of grade, so the check will happen there, but in other situations this can be done by using the result of `executeUpdate()` which returns the number of rows updated.) 

Make sure in case of errors (such as no matching takes tuple), or exceptions, a `connection.rollback()` is issued instead. 

You can use the template given below when writing your code (you may also refer to the file Ass4A_Part2.java on moodle, which builds on the same template).

***What to submit:*** You will submit a Java file *Ass4APart1_<roll_no>.java* with a `main()` that does the above.

**NOTE**: You must write all your code on your own; we will be performing plagiarism detection on your code.

```java
try (java.sql.Connection conn = DriverManager.getConnection(url, user, pass))
{
    conn.setAutoCommit(false);
    try(PreparedStatement stmt1 = …;
       PreparedStatement  stmt2 = …;
       ... )
    {
        ….
        rs1 = stmt1.executeQuery(); // can cause SQLException
        rs2 = stmt2.execute(Query); // can cause SQLException
        conn.commit();     // also can cause SQLException!
    }
    catch(Exception ex) {
        conn.rollback();
        throw ex; /* Other exception handling will be done at outer level */
    } finally {
        conn.setAutoCommit(true);
    }
}
catch(Exception e) {
    e.printStackTrace();
}
```

### Part 2: SQL injection

Create a copy of the instructor table using the following command:

```sql
CREATE TABLE instructordup as SELECT * from instructor;
```

A JDBC program *Ass4A_Part2.java* has been provided to you. This program takes the id of an instructor and updates their salary by 10%. You will find two functions `noPrepStmt(...)` and `withPrepStmt(...)` that perform the above task without using prepared statements and with using prepared statements, respectively; **the latter one is empty,** **you will have to fill in the code**.

Now, run both the functions for the following inputs. You may uncomment the call to the corresponding function in `main()` to test that function. Report your observations for each input.

  1. 10101 (or some name that exists in your `instructordup` table)

2. `’ OR 1=1 --`

3. `’; DROP TABLE instructordup --`

**NOTE: DO NOT try doing this on any production systems in IITB or outside, you may be punished severely.**

**What to submit:** You will submit a Java file *Ass4APart2_<roll_no>.java* with the updated code above, and a text file *Ass4APart2_.txt* with your observations for each input for `noPrepStmt(...)` and `withPrepStmt(...)`. Template is given below:

<pre>
noPrepStmt:
1.
2.
3.
withPrepStmt:
1.
2.
3.
</pre>