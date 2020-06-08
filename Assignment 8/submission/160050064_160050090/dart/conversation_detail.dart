import 'package:flutter/material.dart';
import 'config.dart';
import 'session.dart';
import 'login_page.dart';
import 'dart:convert';
import 'home_page.dart';
import 'create_conversation.dart';


String username, name;

class ConversationDetail extends StatelessWidget {

  ConversationDetail(String uid, String nam) {
    username = uid;
    name = nam;
  }

  @override
  Widget build(BuildContext context) {

    return new MaterialApp(
      title: 'ChatDetail Page',
      home: new Scaffold(
        appBar: new AppBar(
          title: new Text(
            name,
          ),
          actions: <Widget>[
            new IconButton(
              icon: new Icon(Icons.home),
              onPressed: () {
                Navigator.pushReplacement(
                  context,
                  MaterialPageRoute(builder: (context) => HomePage()),
                );
              },
            ),
            new IconButton(
              icon: new Icon(Icons.create),
              onPressed: () {
                Navigator.pushReplacement(
                  context,
                  MaterialPageRoute(builder: (context) => CreateConversation()),
                );
              },
            ),
            new IconButton(
              icon: new Icon(Icons.exit_to_app),
              onPressed: () {
                Session session = new Session();
                session.get(BASE_URL+"LogoutServlet").then((dynamic res) {
                  Navigator.pushReplacement(
                    context,
                    MaterialPageRoute(builder: (context) => LoginPage()),
                  );
                });
              },
            ),
          ],
        ),
        body: AllMessages(),
      ),
    );
  }
}

class AllMessages extends StatefulWidget {

  @override
  AllMessagesState createState() {
    return AllMessagesState();
  }
}

class AllMessagesState extends State<AllMessages> {

  bool loaded = false;
  List<dynamic> messages;
  GlobalKey<FormState> formKey = new GlobalKey<FormState>();
  String newMessage;


  Widget buildRow(dynamic chat) {

    List<Widget> summary = new List<Widget>();
    summary.add(new Expanded(child: new Text(chat['name'], textAlign: TextAlign.center)));
    summary.add(new Expanded(child: new Text(chat['text'], textAlign: TextAlign.center)));
    summary.add(new Expanded(child: new Text(chat['timestamp'], textAlign: TextAlign.center)));

    return Row(
        children: summary,
      );
  }

  Widget buildConversations() {

    return ListView.builder(
        padding: const EdgeInsets.all(16.0),

        itemBuilder: (context, i) {
          if (i.isOdd) return Divider();
          final index = i ~/ 2;
          if(index < messages.length) {
            return buildRow(messages[index]);
          }
        }
    );
  }

  @override
  void initState() {

    Session session = new Session();
    session.get(BASE_URL+"ConversationDetail?other_id="+username).then((dynamic res) {
      Map decoded = json.decode(res);
      if(decoded["status"]) {
        setState(() {
          loaded = true;
          messages = decoded['data'];
        });
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    if(this.loaded) {
      return new Column(

          children: <Widget>[

            new Expanded (
              child: Container(
                child: buildConversations(),
              ),
            ),

            new Container(
              child: new Form(
              key: this.formKey,
                child: new Row(
                children: <Widget>[
                  new Expanded (
                      child: new TextFormField(
                      validator: (value) {
                        if (value.isEmpty) {
                          return 'message cannot be empty';
                        }
                      },

                        decoration: InputDecoration(
                          hintText: 'Type a message',
                            border: OutlineInputBorder(),
                        ),

                      autofocus: false,
                      onSaved: (String value) {
                        this.newMessage = value;
                      },

                  ),
                  ),

                  new RaisedButton(
                    color: Colors.white70,
                    child: new Text(
                      'send',
                      style: new TextStyle(
                        color: Colors.black,
                        fontSize: 18.0,
                      ),
                    ),


                    onPressed: () {

                      if (this.formKey.currentState.validate()) {
                        formKey.currentState.save();

                        Session session = new Session();
                        session.post(BASE_URL+"NewMessage",
                            { "other_id": username,
                              "msg":this.newMessage}).then((dynamic res) {

                                Map decoded = json.decode(res);
                                if(decoded['status']) {
                                  Session session = new Session();
                                  session.get(BASE_URL+"ConversationDetail?other_id="+username).then((dynamic res) {
                                    Map decoded = json.decode(res);
                                    if(decoded["status"]) {
                                      setState(() {
                                        messages = decoded['data'];
                                      });
                                    }
                                  });
                                }
                        });

                      }
                    },
                  ),

                  ],
                ),
            ),
            ),

          ]

      );
    }
    else {
      return new Center(
        child: Text('Loading'),
      );
    }
  }
}
