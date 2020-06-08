
import java.io.IOException;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

/**
 * Servlet implementation class Home
 */
@WebServlet("/Home")
public class Home extends HttpServlet {
	private static final long serialVersionUID = 1L;
       
    /**
     * @see HttpServlet#HttpServlet()
     */
    public Home() {
        super();
        // TODO Auto-generated constructor stub
    }

	/**
	 * @see HttpServlet#doGet(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		
		HttpSession session = request.getSession();
		if(session.getAttribute("id") == null) { //not logged in
			response.sendRedirect("LoginServlet");
		}
		
		String html = "<html><head><title>Home</title>" + 
				"    <script src=\"jquery-3.3.1.js\"> </script>" + 
				"    <script src=\"jquery.dataTables.min.js\"></script>" + 
				"    <script src=\"jquery-ui.min.js\"></script>" + 
				"    <link rel=\"stylesheet\" href=\"jquery-ui.css\" />" + 
				"    <link rel=\"stylesheet\" href=\"jquery.dataTables.min.css\"/>" + 
				
				"	 <script src=\"whatasap_home.js\"></script>" +
				"</head>" + 
				"<body>" + 
				"    <tr><td><button onclick=\"location.href = 'Home';\">Home</button></td>" +
				"    <td><button onclick=\"createConversation()\">Create Conversation</button></td>" +
				"    <td><button onclick=\"location.href = 'LogoutServlet';\">Logout</button></td></tr>"+
				"	<td>"+
				"<div class=\"ui-widget\">" +
                "<input id=\"tags\" name=\"tags\">" +
                "<button onclick=\"searchServe()\">Search</button></div></td><br><br>" +
				"    <div id=\"content\">" +
				"	 </div> <br><br>" + 
				"</body>" + 
				"</html>";
		response.setContentType("text/html");
		response.getWriter().print(html);
	}  
	/**
	 * @see HttpServlet#doPost(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		// TODO Auto-generated method stub
		doGet(request, response);
	}

}
