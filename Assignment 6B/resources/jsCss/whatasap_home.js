/**
 * Sample javascript file. Read the contents and understand them, 
 * then modify this file for your use case.
 */

var myTable;
$(document).ready(function() {
	myTable = $("#usersTable").DataTable({
        columns: [{data:"uid"}, {data:"name"}, {data:"phone"}]
    });
    
    $('#usersTable tbody').on( 'click', 'tr', function () {
        if ( $(this).hasClass('selected') ) {
            $(this).removeClass('selected');
        }
        else {
            myTable.$('tr.selected').removeClass('selected');
            $(this).addClass('selected');
        }
        
        $('#content').html(myTable.row(this).data()["uid"]);
    } );
    
    //load div contents asynchronously, with a call back function
    alert("Page loaded. Click to load div contents.");
	$("#content").load("content.html", function(response){
		//callback function
		alert("Div loaded. Size of content: " + response.length + " characters.");
	});
});

function loadTableAsync() {
    myTable.ajax.url("UsersInfo").load();
}