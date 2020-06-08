/**
 * Sample javascript file. Read the contents and understand them, 
 * then modify this file for your use case.
 */

var myTable;


$(document).ready(function() {
	
	loadTableAsync();
	
    $('#AllConversationsTable tbody').on( 'click', 'tr', function () {
        if ( $(this).hasClass('selected') ) {
            $(this).removeClass('selected');
        }
        else {
            myTable.$('tr.selected').removeClass('selected');
            $(this).addClass('selected');
        }
        
        
        var s = myTable.row(this).data()["uid"];
        
        loadConversationDetailTable(s);
        
    });
    
    
	$( "#tags" ).autocomplete({
			minLength: 1,
			source: "AutoCompleteServlet",
	    });
	
	$( "#ntags" ).autocomplete({
 		minLength: 1,
 		source: "AutoCompleteServlet",
 	});
	
});


function searchServe() {
	var s = document.getElementById("tags").value;
	loadConversationDetailTable(s);
}


function searchServe2() {
	
	var  xhttp = new XMLHttpRequest();
	var s = document.getElementById("ntags").value;
	
	xhttp.onreadystatechange = function() {
	if (this.readyState == 4 && this.status == 200) {
 		
        var obj1 = JSON.parse(this.responseText);
       
        if(obj1.status == true) {
     	   alert("created a conversation successfully");
     	   
     	   var txt = "<form method=\"post\">" 
	  			+ "Message: <input type=\"text\" name=\"msg\"> <br><br>" 
	  			+ "<input type=\"hidden\" name=\"other_id\" value=\""+s+"\" />"
	  			+ "<input type=\"button\" value=\"send\" onClick=\"newMessage(this.form)\"></form>" ;
     	   
     	  document.getElementById("content").innerHTML = txt;
     	  
        }
        else {
        	alert("failed to create a conversation");
        }
      }

    };
	
	
	xhttp.open("GET", "CreateConversation?other_id="+s, true);
    xhttp.send();
}

function createConversation() {
	document.getElementById("content").innerHTML =  "create a new conversation<br><br>"+
	"<div class=\"ui-widget\">" +
    "<input id=\"ntags\" name=\"ntags\">" +
    "<button onclick=\"searchServe2()\">Search</button></div>";
	
	 $( "#ntags" ).autocomplete({
  		minLength: 1,
  		source: "AutoCompleteServlet",
  	});
  	 
}

function loadTableAsync() {
	
	document.getElementById("content").innerHTML = 
		"<table id=\"AllConversationsTable\" class=\"display\">" + 
		"<thead>" + "<tr> <th>User ID</th> <th>Last TimeStamp</th> <th>No. of Messages</th> </tr>" + 
		"</thead>" + "</table>";
		myTable = $("#AllConversationsTable").DataTable({
	        columns: [{data:"uid"}, {data:"last_timestamp"}, {data:"num_msgs"}]
	    });
    myTable.ajax.url("AllConversations").load();
}




function loadConversationDetailTable(s) {
 var  xhttp = new XMLHttpRequest();
     
     xhttp.onreadystatechange = function() {

         if (this.readyState == 4 && this.status == 200) {
     		
           var obj1 = JSON.parse(this.responseText);
           var obj = obj1.data;
           var txt = "<table id=\"ConversationDetailTable\" class=\"display\"><thread><tr><th>User ID</th><th>timestamp</th><th>Message</th></tr></thread>";
           for(var i=0;i<obj.length ;i++) {
         	  txt = txt + "<tr><td>"+obj[i].uid+"</td><td>"+obj[i].timestamp+"</td><td>"+obj[i].text+"</td></tr>";
           }
           txt = txt + "</table><br><br>";
           
           var newMessageForm = "<form method=\"post\">" 
	  			+ "Message: <input type=\"text\" name=\"msg\"> <br><br>" 
	  			+ "<input type=\"hidden\" name=\"other_id\" value=\""+s+"\" />"
	  			+ "<input type=\"button\" value=\"send\" onClick=\"newMessage(this.form)\"></form>" ;
   
           txt = txt + newMessageForm;
           
           if(obj1.status == true) {
         	  if(obj.length==0) {
         		  document.getElementById("content").innerHTML = "No Messages<br><br>"+newMessageForm;  
         	  }
         	  else {
         		  document.getElementById("content").innerHTML = txt;
         	  }
         	    
           }
           else {
         	  document.getElementById("content").innerHTML = "Error while fetching conversation";
           }
           
         }

       };
     
     xhttp.open("GET", "ConversationDetail?other_id="+s, true);
     xhttp.send();

}


function newMessage (form) {
    var other_id = form.other_id.value;
    var msg = form.msg.value;
    
    var xhttp; 
    xhttp = new XMLHttpRequest();
    
    
    xhttp.onreadystatechange = function() {
    	
        if (this.readyState == 4 && this.status == 200) {
        
          var obj = JSON.parse(this.responseText);

          if(obj.status == true) {
        	  loadConversationDetailTable(other_id);
          }
          else {
        	    alert("Unable to send message");
          }
          
        }

      };
    
    xhttp.open("GET", "NewMessage?other_id="+other_id+"\&msg="+msg, true);
    xhttp.send();
    
}
