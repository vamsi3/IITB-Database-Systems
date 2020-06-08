## Assignment 5A: Servlets (In-Class)

Set up your Servlet/JSP project on Eclipse. Now you are all set to actually work on your assignment.  For the in-class assignment you have to do the following:

1. Create a table `password(ID varchar(10), password varchar(20))` in your database.

2. Fill sample user ID/password in the table, with ID matching data from the student and instructor tables in the sample University data you already have.

3. Create an index.html page (using New > HTML page) with some information content (_e.g._ your roll number and name), and a Login link; the login link  simply refers to the Login servlet below, without passing any form parameters.

4. Create a Login servlet with a `doGet()` and `doPost()` method that allows a user to login with a login/password.

5. 1. The `doGet()` method simply calls `doPost(request, response)`
   2. The `doPost()` method checks if the parameters ID and password parameters are not `null`; if either is `null`, it displays a form to fill in the parameters and invoke the same servlet using `doPost()`, on submit. 
   3. If both parameters are not `null`, it checks if the ID/password match with the user table in the database.
      - NOTE: We are storing passwords in plain text.  You should NEVER do this in the real world, instead the system should concatenate the password with random text stored in a "salt" attribute of the password table, compute a hash on on the concatenated test, and store the hash in the user table (when setting password), or compare the hash with the stored value in the user table (when checking the password).

6. On failed authentication, it displays an error message and displays the form as above.

7. On successful authentication of the password, it sets a session variable to store the ID, and displays a home page by redirecting to a Home servlet:  Use `response.sendRedirect("Home")` to do this (you may need to add the project name before Home, _e.g._ `Ass5A/Home`, if your project is called `Ass5A`).

8. Create a Home servlet that displays the following:

9. 1. The users name and department, which is retrieved based on the ID value stored in the session
   2. If the user is a student, show a link to a servlet `displayGrades`, which displays the student’s grades for all the courses the student has taken.  It should take the ID from the session, and display the course_id, title, section id, semester, year, and the grade obtained.
   3. a logout button, which links to a Logout servlet, which calls `session.invalidate()` to logout the user, and redirects to the Login servlet.

10. All servlets other than the Login servlet should check for authentication via the session variable before doing any other processing, and should redirect to the login servlet in case the user is not logged in.



Submit a zip or tar file containing all the files that you have created. 

1. _Make sure to name the directory by your roll number, so expanding the files will create separate folders for different students._
2. _Make sure the database host, name, password etc. are defined only once in static variables so your TAs can easily change them to test your program._