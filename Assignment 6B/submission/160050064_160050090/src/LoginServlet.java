

import java.io.IOException;
import java.io.PrintWriter;
import java.util.List;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

/**
 * Servlet implementation class LoginServlet
 */
@WebServlet("/LoginServlet")
public class LoginServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;
	
    private static final String LOGIN_FORM = 
    		"<html><body><form id=\"loginform\" method=\"post\">" 
			+ "        ID: <input type=\"text\" name=\"userid\"> <br><br>" 
			+ "        Password: <input type=\"password\" name=\"password\"> <br><br>"
			+ "        <input type=\"submit\" value=\"Login\">" 
			+ " </form></body></html>";
	
    /**
     * @see HttpServlet#HttpServlet()
     */
    public LoginServlet() {
        super();
        // TODO Auto-generated constructor stub
    }

	/**
	 * @see HttpServlet#doGet(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		// TODO Auto-generated method stub
		response.setContentType("text/html");
		PrintWriter out = response.getWriter();
		out.println(LOGIN_FORM);
	}

	/**
	 * @see HttpServlet#doPost(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doPost(HttpServletRequest request, HttpServletResponse response) 
			throws ServletException, IOException {
		// TODO Auto-generated method stub
		String userid = request.getParameter("userid");
		String password = request.getParameter("password");
		
		String query = "select password from password where uid = ?";
		List<List<Object>> res = DbHelper.executeQueryList(query, 
				new DbHelper.ParamType[] {DbHelper.ParamType.STRING}, 
				new Object[] {userid});
		
		String dbPass = res.isEmpty()? null : (String)res.get(0).get(0);
		if(dbPass != null && dbPass.equals(password)) {
			HttpSession session = request.getSession();
			session.setAttribute("id", userid);
			response.sendRedirect("Home");
		}
		else {
			response.setContentType("text/html");
			response.getWriter().print("<p><b>Auth failed. Try again</b></p>");
			response.getWriter().print(LOGIN_FORM);
		}
	}

}
