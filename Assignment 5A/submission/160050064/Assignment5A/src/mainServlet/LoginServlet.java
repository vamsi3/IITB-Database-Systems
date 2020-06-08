package mainServlet;

import java.io.IOException;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.*;
import java.io.PrintWriter;
import java.sql.*;


@WebServlet("/LoginServlet")
public class LoginServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;
	private static final String databaseUrl = "jdbc:postgresql://localhost:5640/test";
	private static final String databaseUsername = "labuser";
	private static final String databasePassword = "";

    public LoginServlet() {
        super();
    }

	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		doPost(request, response);
	}

	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		HttpSession session = request.getSession(false);
		if (session != null) {
			response.sendRedirect("Home");
		}
		String id = request.getParameter("id");
		String password = request.getParameter("password");
		if (id == null || password == null) {
			response.setContentType("text/html");
			PrintWriter out = response.getWriter();
			showLoginForm(out);
			return;
		}
		else {
			if (checkAuth(id, password)) {
				session = request.getSession(true);
				session.setAttribute("ID", id);
				response.sendRedirect("Home");
			}
			else {
				response.setContentType("text/html");
				PrintWriter out = response.getWriter();
				out.println("<p>Entered credentials invalid. Login failed! Please try again.</p>");
				showLoginForm(out);
			}
		}
	}

	private void showLoginForm(PrintWriter out) {
		out.println("<form action=\"LoginServlet\" method=\"POST\">Enter your ID:<input type=\"text\" name=\"id\">Enter your password:<input type=\"password\" name=\"password\"><input type=\"submit\" value=\"Submit\"></form>");
		return;
	}

	private boolean checkAuth(String id, String password) {
		try (java.sql.Connection conn = DriverManager.getConnection(databaseUrl, databaseUsername, databasePassword)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement authQuery = conn.prepareStatement("SELECT password FROM password WHERE ID=?");
			) {
				authQuery.setString(1, id);
				ResultSet rset = authQuery.executeQuery();
				if (!rset.next()) {
					// ID not found in database
					return false;
				}
				String dbPassword = rset.getString(1);
				if (password.equals(dbPassword)) return true;
				else return false;
			}
			catch (SQLException ex) {
				throw ex;
			}
			finally {
				conn.setAutoCommit(true);
			}
		}
		catch (SQLException ex) {
			return false;
		}
	}
}
