package whatasap;

import java.io.IOException;
import java.io.PrintWriter;
import java.sql.*;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

@WebServlet("/Login")
public class LoginServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;

    public LoginServlet() {
        super();
    }

	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		doPost(request, response);
	}

	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		HttpSession session = request.getSession(false);
		if (session != null) response.sendRedirect("Home");

		String id = request.getParameter("id");
		String password = request.getParameter("password");
		if (id == null || password == null) {
			response.setContentType("text/html");
			PrintWriter out = response.getWriter();
			addLoginForm(out);
			return;
		}

		if (checkAuth(id, password)) {
			session = request.getSession(true);
			session.setAttribute("id", id);
			response.sendRedirect("Home");
		}
		else {
			response.setContentType("text/html");
			PrintWriter out = response.getWriter();
			out.println("<html><body>");
			out.println("<p>Entered credentials invalid. Login failed!</p>");
			addLoginForm(out);
		}
	}
	
	private void addLoginForm(PrintWriter out) {
		out.println("<div align=\"center\">");
		out.println("<form action=\"Login\" method=\"POST\">Enter your ID:<input type=\"text\" name=\"id\"> <br> Enter your password:<input type=\"password\" name=\"password\"> <br> <input type=\"submit\" value=\"Login\"></form>");
		out.println("</div></body></html>");
	}
	
	private boolean checkAuth(String id, String password) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement authQuery = conn.prepareStatement("SELECT password FROM password WHERE id=?");
			) {
				authQuery.setString(1, id);
				ResultSet rset = authQuery.executeQuery();
				if (!rset.next()) return false;
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
			ex.printStackTrace();
			return false;
		}
	}
}
