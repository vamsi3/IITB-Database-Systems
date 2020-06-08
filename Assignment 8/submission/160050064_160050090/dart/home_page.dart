import 'package:flutter/material.dart';
import 'config.dart';
import 'session.dart';
import 'login_page.dart';
import 'dart:convert';
import 'conversation_detail.dart';
import 'create_conversation.dart';

class HomePage extends StatelessWidget {

  @override
  Widget build(BuildContext context) {

    return new MaterialApp(
      title: 'Home Page',
      home: new Scaffold(
        appBar: new AppBar(
          title: new Text(
            'Chats',
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
        body: AllConversations(),
      ),
    );
  }
}

class AllConversations extends StatefulWidget {
  @override
  AllConversationsState createState() {
    return AllConversationsState();
  }
}

class AllConversationsState extends State<AllConversations> {

  bool loaded = false;
  List<dynamic> conversations;
  List<dynamic> allconversations;
  //List<dynamic> tempMap;
  //String term = "";

  Widget buildRow(dynamic chat) {

    List<Widget> summary = new List<Widget>();
    summary.add(new Expanded(child: new Text(chat['name'], textAlign: TextAlign.center)));
    String timestamp = "No Messages";
    if(chat['last_timestamp'] != "2000-01-01 00:00:00.0") {
      timestamp = chat['last_timestamp'];
    }
    summary.add(new Expanded(child: new Text(timestamp, textAlign: TextAlign.center)));
    summary.add(new Expanded(child: new Text(chat['num_msgs'].toString(), textAlign: TextAlign.center)));

    return InkWell(
     child: new Row(
       children: summary,
     ),
     onTap: () {
       Navigator.pushReplacement(
         context,
         MaterialPageRoute(builder: (context) => ConversationDetail(chat['uid'],chat['name'])),
       );
     },
    );
  }

  Widget buildConversations() {

    return ListView.builder(
        padding: const EdgeInsets.all(16.0),

        itemBuilder: (context, i) {

          final index = i ~/ 2;
          if(index < conversations.length) {
            if (i.isOdd) return Divider();
            return buildRow(conversations[index]);
          }
        }
    );
  }

  @override
  void initState() {

    Session session = new Session();
    session.get(BASE_URL+"AllConversations").then((dynamic res) {
      Map decoded = json.decode(res);
      if(decoded["status"]) {
        setState(() {
          loaded = true;
          conversations = decoded['data'];
          allconversations = conversations;
        });
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    if(this.loaded) {
      return new Column(

        children: <Widget>[
          new TextField(
            decoration: InputDecoration(
              hintText: 'Search',
                border: OutlineInputBorder(),
            ),
            onChanged: (text) {

              if(text == "") {

                setState(() {
                  conversations = allconversations;
                });
              }
              else {

                List<dynamic> temp = new List<dynamic>();
                for (var i = 0; i < allconversations.length; i++) {
                 
                  if(allconversations[i]['name'].toLowerCase().contains(text.toLowerCase()) ||
                      allconversations[i]['uid'].toLowerCase().contains(text.toLowerCase()))
                  {
                    temp.add(allconversations[i]);
                  }
                }

                setState(() {
                  conversations = temp;
                });
              }
            },
          ),
          new Expanded (
            child: Container(
              child: buildConversations(),
            ),
          ),

        ],
      );
    }
    else {
      return new Center(
        child: Text('Loading...'),
      );
    }
  }
}
