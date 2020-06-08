import 'package:flutter/material.dart';
import 'config.dart';
import 'session.dart';
import 'login_page.dart';
import 'dart:convert';
import 'home_page.dart';
import 'conversation_detail.dart';
import 'package:flutter_typeahead/flutter_typeahead.dart';



class CreateConversation extends StatelessWidget {


  @override
  Widget build(BuildContext context) {

    return new MaterialApp(
      title: 'CreateConversation Page',
      home: new Scaffold(
        appBar: new AppBar(
          title: new Text(
            'Create Conversation',
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
                  MaterialPageRoute(builder: (context) => HomePage()),
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
        body: SearchUser(),
      ),
    );
  }
}

class SearchUser extends StatefulWidget {

  @override
  SearchUserState createState() {
    return SearchUserState();
  }
}

class SearchUserState extends State<SearchUser> {


  @override
  Widget build(BuildContext context) {
    return TypeAheadField(
      textFieldConfiguration: TextFieldConfiguration(
          autofocus: true,

          decoration: InputDecoration(
              border: OutlineInputBorder()
          )
      ),
      suggestionsCallback: (pattern) async {

        Session session = new Session();
        String res = await session.get(BASE_URL+"AutoCompleteUser?term="+pattern);
        var decoded = json.decode(res);
        return decoded;

      },
      itemBuilder: (context, suggestion) {
        return ListTile(
          title: Text(suggestion['label']),
        );
      },
      onSuggestionSelected: (suggestion) {

        Session session = new Session();
        session.get(BASE_URL+"CreateConversation?other_id="+suggestion['value']).then((dynamic res) {

          Map decoded = json.decode(res);
          String tempname = suggestion['label'].toString().split(',')[0].substring(5);
          if(decoded['status']) {
            Navigator.pushReplacement(
              context,
              MaterialPageRoute(builder: (context) => ConversationDetail(suggestion['value'],tempname)),
            );
          }
          else {
            Scaffold.of(context)
                .showSnackBar(SnackBar(content: Text('Could not create a conversation')));
          }
        });

      },
    );
  }
}
