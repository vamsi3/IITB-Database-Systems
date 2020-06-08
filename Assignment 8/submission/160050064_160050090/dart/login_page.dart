import 'package:flutter/material.dart';
import 'session.dart';
import 'dart:convert';
import 'home_page.dart';
import 'config.dart';


class LoginData {
  static String username = '';
  static String password = '';
  static bool   isLoggedIn = false;
}

class LoginPage extends StatelessWidget {

  @override
  Widget build(BuildContext context) {

    return MaterialApp(
      title: "WhatASap",
      home: Scaffold(
        appBar: AppBar(
          title: Text("Login"),
        ),
        body: LoginForm(),
      ),
    );
  }
}

class LoginForm extends StatefulWidget {
  @override
  LoginFormState createState() {
    return LoginFormState();
  }
}

class LoginFormState extends State<LoginForm> {

  final GlobalKey<FormState> formKey = new GlobalKey<FormState>();


  @override
  Widget build(BuildContext context) {

    final username = TextFormField(
      validator: (value) {
        if (value.isEmpty) {
          return 'username cannot be empty';
        }
      },

      autofocus: false,
      onSaved: (String value) {
        LoginData.username = value;
      },
      decoration: InputDecoration(
        hintText: 'Username',
      ),
    );

    final password = TextFormField(
      autofocus: false,
      obscureText: true,
      onSaved: (String value) {
        LoginData.password = value;
      },
      decoration: InputDecoration(
        hintText: 'Password',
      ),
    );

    final loginButton = new RaisedButton(
      color: Colors.lightBlueAccent,
      child: new Text(
        'Login',
        style: new TextStyle(
          color: Colors.white,
          fontSize: 18.0,
        ),
      ),


      onPressed: () {

        if (this.formKey.currentState.validate()) {
          formKey.currentState.save();

          Session session = new Session();
          session.post(BASE_URL+"LoginServlet",
              { "userid": LoginData.username,
                "password": LoginData.password}).then((dynamic res) {
            Map decoded = json.decode(res);
              if(decoded['status']) {
                LoginData.isLoggedIn = true;
                Navigator.pushReplacement(
                  context,
                  MaterialPageRoute(builder: (context) => HomePage()),
                );
              }
              else {
                Scaffold.of(context)
                    .showSnackBar(SnackBar(content: Text(decoded['message'])));
              }
          });
        }
      },
    );


    return Form(
      key: this.formKey,
      child: new ListView(
        padding: EdgeInsets.only(top: 30.0, left: 24.0, right: 24.0),
        children: <Widget>[

          username,
          new SizedBox(height: 34.0),
          password,
          new SizedBox(height: 34.0),
          loginButton,
        ],
      ),
    );
  }

}