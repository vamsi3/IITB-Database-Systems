## Assignment 5b: Servlets (Take-Home) - WhatASap 

Preliminary to the assignment:  Read up about sequences and the serial type in PostgreSQL from https://www.postgresql.org/docs/10/static/sql-createsequence.html. Read the Examples section first, before reading the earlier part of this page.



Your goal is to implement a simple WhatsApp-like application which we'll call WhatASap. To this end, you do the following:

1. Write a script `create.sql` to create tables. The schema is described below.

   - ```sql
      create table users(
       uid varchar(10) primary key,
       name varchar(20),
       phone varchar(10));
      ```
      
   - ```sql
      create table conversations(
        uid1 varchar(10) references users,
        uid2 varchar(10) references users,
        thread_id serial,
        primary key (uid1, uid2),
        unique(thread_id), 
        check (uid1 < uid2));
      ```
      - Note 1: Since a conversation is shared between two users and should not depend on the order in which the uids appear, ensure uid1 < uid2 by swapping the IDs if they are not in ascending order.
      
      - Note 2: thread_id is defined as type serial, which creates a new value for each tuple that is inserted into conversations.
   
   - ```sql
      create table posts (
       post_id serial primary key,
       thread_id integer references conversations(thread_id),
       uid varchar(10) references users,
       timestamp timestamp,
       text varchar(256)
     );
     ```
     
   - Also fill in data for user and follows to demonstrate the interfaces described below.  Create a file `data.sql` with insert statements for sample data.
   
   - You can use auto increment annotations or sequences, instead of the `serial` type fields, if you want. 
   
3. When a user logs in (use the login code from the assignment 5A), they are directed to a home page which is defined as below.

3. The home page displays:

   1. A form to create new conversations, which takes in a user ID and invokes a servlet `createConversation`, which creates a new conversation. Assume for simplicity that all users present in the users table are automatically registered on WhatASap.  When a user ~~name~~ ID is input, a new conversation is created with that user (if it does not already exist). A confirmation/error message is displayed, along with a link to go back to the home page.

   2. Display an entry for each of the user’s conversations, along with the other user’s name, last message (if any) in that conversation, and its timestamp. Conversations can be displayed with most recent first; make sure nulls (conversations without any messages) are sorted last.
      Each entry should also have a link to show `ConversationDetails` as described below, with `threadid` as a GET parameter.

   3. `ConversationDetails`: A servlet that displays all the messages in the conversation passed as GET parameter, sorted by their timestamp in decreasing order. The servlet also contains a form to create a new message, by invoking a `NewMessage` servlet. The Servlet should check that the user is part of the conversation.

   4. The New Message servlet posts the submitted message to the conversation. Upon successful submit, the servlet redirects back to the `ConversationDetails` servlet, which shows the conversation updated with the new message.

   5. Again, make sure that all servlets other than the Login servlet should check for authentication via the session variable before doing any other processing, and should redirect to the login servlet in case the user is not logged in.

   6. ***Bonus: (not required, do it only if you want, can compensate for marks lost elsewhere, but marks will not exceed 100% in any case)***
      Remember the last message in a conversation that was displayed, and show the list at that point. To do so, make your display of messages as table with a scroll bar, and use the following Javascript code to scroll to the desired message:

      ```html
      <head>
      	<style>
        		.scroll tbody { display:block; height:200px; overflow:auto;}
      	</style>
          <script>
              window.onload = function() {
                  document.getElementById("50").scrollIntoView();
      		};
      	</script>
</head>
      ....
<tr id=50> ... </tr>
      ....
</table>
      ```



**Note 1: Make sure to avoid SQL injection vulnerabilities AND ensure that every servlet checks for an active session at the beginning**

**Note 2: Use the method you created in the last assignment for printing out ResultSets as HTML tables. Otherwise you will be writing the same code many times.** 



Submit a zip or tar file containing all the files that you have created.  Make sure to name the directory by your roll number, so expanding the files will create separate folders for different students.

