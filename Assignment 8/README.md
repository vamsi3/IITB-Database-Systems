## Assignment 8: Android App Development with Flutter

In this assignment you will build an Android App for WhatASap.  This will be a variant of the JavaScript version of the social network which you built earlier.  We will provide the main servlets for the backend work, and you have to handle the frontend work using the Flutter framework.

**NOTE 1:  This assignment is to be done in groups of 2**

**NOTE 2:  There's lots of new stuff to learn.  Feel free to discuss how to do things with friends.  But the code you submit must be written by you.**



1. The backend will be a set of servlets provided by us, accessed using HTTP, and returning JSON. Your app will make use of the JSON that is returned by the servlets. You should not make any changes to the servlets that we provide.

2. The app starts with a login page. The user will enter their username and password, which upon submit you will send to the backend servlet for authentication. Provide the following functionality:

   1. Use Flutter’s form verification to verify that the username entered by the user is not empty.

   2. Use Flutter’s Snackbar to show a “Login failed” message in case the authentication fails.
      *Hint: Widgets in Flutter are organized as a tree. Each Stateful Widget in Flutter has a "build context", which can be used to locate the Widget in the tree. Each child has access to the parent's build context. You should use the build context of the main Scaffold for the Login screen to show your Snackbar. If your Snackbar isn't showing, you are probably using the wrong Snackbar.
      To fix this, use a Builder widget and explicitly pass the required context to children. Example below:*

      ```dart
   @override
      Widget build(BuildContext context) {
       return Scaffold(
            appBar: AppBar(
        title: Text('Login Page'),
            ),
      body: Builder(builder: (scaffoldContext) {
              //this is the context of the parent Scaffold that you should use to show Snackbar
        // ...
              // Scaffold.of(scaffoldContext).showSnackBar(yourSnackBar);
        // ...
            }));
      }
      ```

   3. Use the dart class `Session` that we have provided to make get and post requests to the backend servlets, which takes care of session management using cookies.

      - Flutter does not natively support session management using cookies, which is why we have provided the `Session` class.

      - The `Session` class that we have provided is a singleton. So, you can use `new Session()` multiple times and still obtain the same session object.

3. If the authentication succeeds, the user should be taken to a Chats screen that shows all the chat summaries (other user **name (added on 24/9)** id and last timestamp of conversation, as returned by the `AllConversations` servlet) of the user. Implement the following in the Chats screen:
   1. The screen should be titled “Chats” and should also provide two icons on the title bar: a home icon, a create conversation icon, and an exit icon. Tapping the “home” icon will take the user to the “Chats” screen (default). Tapping the “create” icon will take the user to a “Create Conversation” screen (described below). Tapping the “exit” icon will log the user out of the app.
      *Hint: You can use `Icons.home`, `Icons.create` and `Icons.exit_to_app` provided by Flutter to get nice looking icons for the above.*
   2. Use Dart `async-await` to load the chats. While the chat is still loading, the Chats screen should display the text “Loading …”. Once the chat summaries are available, they are shown.
      *Hint: Initialize the Chats screen content to show the loading text, and override `initState` method to call an asynchronous function that fetches the chats. You can use the sleep function from the package `dart.io` to artificially introduce a delay to test your loading text.*
   3. The Chats screen also has a search box. Initially the Chats screen displays all chat summaries for each user the logged in user had conversations with. As the user types in some text in the search box, the Chats screen should be updated to show the chat summaries for only those users whose **name or (added 24/9)** id contains the given text.
      *Hint: Use a `TextField` with `onChanged` event that filters the chat summaries based on the text typed so far. Alternatively you can use a `TextFieldController`, if you use a `TextFormField`.*

4. Tapping on a chat summary should load a new screen (lets call it ChatDetail) with all the messages in that chat in ascending order (the latest message in the conversation shows up last).
   1. Use the **name** ~~id~~ **(updated 24/9)** of the other user in the conversation as the title of the ChatDetail screen.
   2. The ChatDetail screen should also allow the user to type a new message and send. Once the message is sent, the screen should be reloaded with the updated messages.
      *Note: Do not worry about auto-scrolling the messages after the new message has been submitted. (Ideally, this should be done, but Flutter has a bug, which needs an elaborate workaround to achieve this. For this assignment, you need not implement this.)*
   3. Make sure the text box for new message stays in position even while you scroll through the messages.
   4. Also, the text box should appropriately slide itself above the keyboard as the keyboard appears.
      *Hint: Consider using a `ListView` inside an Expanded, and then the textbox to type new messages. This lets the list take up the maximum amount of space before the textbox below it can be rendered and allows smooth transition when keyboard appears.*
5. The “Create Conversation” screen should contain a search box to search for a user based on their id, name or phone. As the user types in a term, the auto complete list should show the id, name and phone number of each user whose id, name or phone number is prefixed by the text typed so far, by using the `AutoCompleteUser` servlet we have provided.
   1. You can use the package `flutter_typeahead` to achieve auto complete.
   2. Upon selecting an option from the autocomplete list, your app should create a new conversation using the backend `CreateConversation` servlet and take the user to a ChatDetail screen to start chatting. (If a conversation with the selected user already exists, simply take the user to the ChatDetail screen with existing messages.)
      *Hint: Use `itemBuilder` to build the suggestion text from the obtained JSON. Use `onSuggestionSelected` to process the selected suggestion.*

**SUBMISSION INSTRUCTIONS:**

1. Submit all your dart files as well as your `pubspec.yaml` file. Add them to a single directory, compress it and submit it.
2. Make sure machine IP/port numbers are stored in only one place in your program, so they can be easily edited.
3. Add a README file with instructions on how to setup and run your code.
4. Name the directory based on your roll numbers. *e.g.* 150050001_150050012.tar.gz
5. The assignment will be accepted for 3 days beyond the due date, with a penalty of 10% per day. After that no more submissions will be accepted.

