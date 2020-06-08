## Assignment 6B: Javascript / AJAX (Take-Home)

**This assignment can be done in groups of 2.**

**NOTE: There's lots of new stuff to learn.  Feel free to discuss how to do things with friends.  But the code you submit must be written by you.**

In this assignment you will use JavaScript / AJAX with jQuery to extend the WhatASap chatting application you created last week with a better interface.

### Prerequisites

Modern applications are written as a Web service, which your front end connects to to get data and perform updates. We have done half of your assignment for you by providing servlet code for the WhatASap application which implements the required web services for your assignments; all the servlets return results in JSON format as detailed below. You can download the code from moodle.  You will have to modify one of the servlets as detailed later.  

The zip file includes code for the following servlets implementing your web service API. The input and output of each of these servlets is documented below. 

### Servlet API

NOTE: All servlets return a JSON object with these attributes:

- status      :  "true" on success, or "false" on error 
- message :  string with error message;  present only in case of error.
- data         :  JSON array or object containing result of servlet invocation; present only in case of success.



1. **AllConversations**
   Input: session variable "id"
   Output:  status/message, and data: JSON array of all the conversations that the current user is involved in.
   Each conversation has this JSON structure:
   
   ```json
   {
       uid: value,
       last_timestamp: value,
       num_msgs: value
   }
   ```
   
   where `uid` is the id of the other user, and timestamp is a string, which is null if there are no messages, and `num_msgs` is 0 in that case. 
   
2. **CreateConversation**
   Input: session variable `id`, request parameter `other_id`
   Output: Creates a new conversation for those users (if one does not already exist) and returns status and message (if any), no data.

3. **ConversationDetail**
   Input: session variable `id`, request parameter `other_id`
   Output: status/message, and data: JSON array of all messages in the conversation between users with id's `id` and `other_id`, where each message has the structure 

   ```json
   {
       post_id: value,
       thread_id: value,
       uid: value,
       timestamp: value,
       text: value
   } 
   ```


   where `uid` is the id of the person who posted the message (could be the current user) and text is the message content. 

4. **NewMessage**
   Input: session variable `id`, request parameter `other_id`, request parameter `msg`.
   Parameter `other_id` and `msg` denote the user to whom the message should be sent, and the message text.
   Output: status and message only, no data.



We have also provided these:

1. A Config.java file where you need to provide database connection info.
2. A Login servlet, and a Logout servlet, which you can tweak if you want. On successful login, the Login servlet redirects to the Home servlet.
3. A Home servlet which has some sample code to help you get started. 

The schema used by these servlets, including sample data, is in the file `create.sql`, which is provided in the zip file.

### Assignment

1. The entry point to your system is the Login servlet which takes `userid` and `password` as parameters. On successful login, it redirects to the Home servlet.

2. We have provided an empty servlet Home.java which you should fill in. Your JavaScript code should be in a file `whatasap_home.js` included via a `script` tag, and in the Home page servlet output. 

3. You have to edit the `NewMessage` servlet to **sanitize the input** to **prevent cross-site scripting (XSS) attacks**. Input sanitization can be done using the Jsoup Java library as explained here: https://jsoup.org/cookbook/cleaning-html/whitelist-sanitizer (download the library from https://jsoup.org/download)
   In the provided NewMessage.java file, look for a line that says `"//Edit this line to sanitize newMsg before assigning to sanitizedMsg`. This is where you will need to add your code for sanitizing the input.
   
4. The homepage should contain:

   1. A home button, 
   2. A search box, and 
   3. A create conversation link 
   4. A HTML div element in the lower part of the page.

   The details of their functionality are as follows:

   - **Div element:** Use a div tag (*e.g.* `<div id="content">  </div>`) for this part of the page, and the div has no content at all; instead you should fill in the content by using an AJAX call to a servlet `AllConversations` which we have described below; the AJAX call to `AllConversations` is invoked `onLoad()` of the page and replaces the div content by the result of the AJAX call formatted as below. 
     - The `AllConversations` servlet call returns all the conversations in JSON format as described earlier. Once the result is received, your JavaScript code should replace the div of the lower part of the page by the result formatted as a table  
     - The table should be displayed using the JQuery DataTables plugin available at: https://datatables.net/ Specifically, look at: https://datatables.net/examples/ajax/.  
     - Clicking on a `userid` should replace the div element by details of that conversation. The sample Home.java has JavaScript code indicating how to to implement this feature.

   - **Home button:** The home button invokes the Ajax call described above to replace the div element by the table of all conversations as described above (your code should make this table load automatically on page load, but the div may get replaced by other content; the home button should restore the default).

   - **Search box**: The search box works as follows:

     1. Autocomplete on the search box allows the user to type in the ID, name or phone of another user.  As the user is typing in the above, the system should allow autocomplete to display the ID, name and phone of matching users. The autocomplete feature uses a servlet `AutoCompleteUser`, which you should write, which takes a partially typed ID/name/phone, and returns users whose ID/name/phone prefix matches the text typed so far.
        Use the jQuery autocomplete feature for this, documented in https://jqueryui.com/autocomplete/ and [ http://api.jqueryui.com/autocomplete/](http://api.jqueryui.com/autocomplete/).

        Use the version of the interface which takes an array of objects with `label` and `value` properties:`[ { label: "Choice1", value: "value1" }, ... ]`

     2. Once a user is chosen and submitted the div element is replaced by the details of the conversation with that user, as described below.

   - **Conversation Detail:** When a conversation is chosen either from the table or from the search, your JavaScript code should initiate an asynchronous AJAX call to a servlet `ConversationDetail` (which we have provided), which returns all messages in that conversation in JSON format.
     *NOTE: use `XMLHttpRequest` to load the data asynchronously, and use the `onreadystatechange` function to display it when the data is returned.* 
     When the asynchronous data return is complete, the div element of the home page should be replaced by

     1. the list of messages in the conversation, followed by
     2. a form to create new messages. To do so, first use `JSON.parse()` on the data, and then iterate over the JSON array to construct the contents in HTML, and then display the content by using: `document.getElementById("content").innerHTML=  ...` html string that you constructed ...

   - **Create message:** When the form to create a message is submitted, the servlet `NewMessage` is invoked using JavaScript to create a new message. On success, the conversation detail is shown again, updated with the new message. On failure an alert box is shown. 

   - **Create Conversation:** On clicking the Create Conversation link, the lower part of the page is replaced by a form to create a new conversation, using autocomplete as described earlier to choose a user; on submit the JavaScript code invokes the `CreateConversation` servlet to create the conversation. An alert box is shown to let the user know the success or failure status.

   - **Bonus** *(will not take score above 100, but can compensate for errors in other parts of the assignment)*
     Along with each message in a conversation that was sent by the current user, add an X icon, which can be used to delete the message.  You should have a popup that verifies from the user that she wants to delete the message, and then performs the delete using an AJAX call. You have to write a servlet to implement deletion. After the message is deleted refresh the list of messages in the conversation.

**To submit the assignment:**

1. Submit a zip/tar file containing the files outlined below; the main directory name must include your roll numbers. 

2. The project should include 

   1. A README file which includes your names / roll-numbers and explains how to run your code 

   2. All your java/jsp/html/css files (include the directories Eclipse Java Resources > src and WebContent > WEB-INF)

   3. If you added more data to test/show off some features, you have to provide an sql dump.  You can use pg_dump as below:

      ```bash
      /usr/lib/postgresql/10/bin/pg_dump -h localhost -p xyz -d postgres -O > dbdump.sql
      ```


      where xyz is the port where your PostgreSQL is running, and the -d defines the database name (default is postgres).  The -O option above is important to ensure usernames are not included in the dump; the dump also has some unnecessary lines at the beginning, which you can delete, so it starts off with the create table statements, followed by the data.

