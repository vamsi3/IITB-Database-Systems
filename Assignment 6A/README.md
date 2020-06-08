## Assignment 6A: JavaScript (In-Class)

This is a small assignment. It requires you to do the following:

1. Create an eclipse dynamic web project

2. Download the servlets that we have provided and put them in the project

3. Edit Config.java to give your database credentials

4. Download the database ddl/dml that we have provided and, populate the tables

5. Invoke the Login servlet from the Eclipse browser window to login

6. Create a html page by editing the sample code below, to show a list of users (get it from the database), and on selecting a user, 

7. 1. invoke the `conversationDetail` servlet to get the messages for that user,
   2. parse the returned JSON using `JSON.parse(this.responseText)`, and 
   3. display the contents in basic HTML format by assigning to `document.getElementById().innerHTML` as in the sample code below.
   4. NOTE: create a complete well-formed HTML text before assigning to `innerHTML`. If you do it piecewise, the browser thinks your HTML is malformed and does error correction (*e.g.* insert a table close tag which it thinks is missing).

8. Open the html page, select the user and view the result for the user

9. 1. NOTE: You can copy the URL from eclipse to a browser, so you can use browser JavaScript debugging tools

   2. 1. The JS console provided by your browser is *really* helpful in finding bugs.
      2. Also use the inspect function in your browser to view the DOM tree. It's easy to find bugs in DOM tree construction by doing this.

10. Submit the HTML page as your assignment submission. Any name for the file is fine, but you are welcome to append your roll number to it.

HTML Page for you to modify (this is from https://www.w3schools.com/js/tryit.asp?filename=tryjs_ajax_database)

```html
<!DOCTYPE html>
<html>
<style>
table, th, td {
	border: 1px solid black;
	border-collapse: collapse;
}
th, td {
	padding: 5px;
}
</style>

<body>
	<h2>The XMLHttpRequest Object</h2>
	<form action="">
		<select name="customers" onchange="showCustomer(this.value)">
			<option value="">Select a customer:</option>
			<option value="ALFKI">Alfreds Futterkiste</option>
			<option value="NORTS ">North/South</option>
			<option value="WOLZA">Wolski Zajazd</option>
		</select>
	</form>
	<br>
	<div id="txtHint">Customer info will be listed here...</div>
	<script>
	function showCustomer(str) {
		var xhttp;
		if(str == "") {
			document.getElementById("txtHint").innerHTML = "";
			return;
		}
		xhttp = new XMLHttpRequest();
		xhttp.onreadystatechange = function() {
			if(this.readyState == 4 && this.status == 200) {
				document.getElementById("txtHint").innerHTML = this.responseText;
			}
		};
		xhttp.open("GET", "getcustomer.asp?q=" + str, true);
		xhttp.send();
	}
	</script>
</body>
</html>
```

