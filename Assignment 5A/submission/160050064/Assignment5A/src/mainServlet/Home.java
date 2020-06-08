package mainServlet;

import java.io.IOException;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.*;
import java.io.PrintWriter;
import java.sql.*;


@WebServlet("/Home")
public class Home extends HttpServlet {
	private static final long serialVersionUID = 1L;
	private static final String databaseUrl = "jdbc:postgresql://localhost:5640/test";
	private static final String databaseUsername = "labuser";
	private static final String databasePassword = "";

    public Home() {
        super();
    }

	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		HttpSession session = request.getSession(false);
		if (session == null) {
			response.sendRedirect("LoginServlet");
			return;
		}
		String ID = session.getAttribute("ID").toString();

		try (java.sql.Connection conn = DriverManager.getConnection(databaseUrl, databaseUsername, databasePassword)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement studentQuery = conn.prepareStatement("SELECT name, dept_name FROM student WHERE id=?");
				PreparedStatement instructorQuery = conn.prepareStatement("SELECT name, dept_name FROM instructor WHERE id=?");
			) {
				response.setContentType("text/html");
				PrintWriter out = response.getWriter();
				out.println("<html><body>");

				studentQuery.setString(1, ID);
				ResultSet rset = studentQuery.executeQuery();
				if (!rset.next()) {
					instructorQuery.setString(1, ID);
					ResultSet rset2 = instructorQuery.executeQuery();
					if (!rset2.next()) {
						throw new SQLException("No matching data found. Likely an error in database");
					}
					// we now know the ID is a instructor
					String name = rset2.getString(1);
					String dept_name = rset2.getString(2);
					out.println(String.format("<p>Name: %s | Department Name: %s</p>", name, dept_name));
				}
				else {
					// we now know the ID is a student
					String name = rset.getString(1);
					String dept_name = rset.getString(2);
					out.println(String.format("<p>Name: %s | Department Name: %s</p>", name, dept_name));
					out.println("<a href=\"DisplayGrades\">Display Grades</a>");
				}

				out.println("<a href=\"LogoutServlet\"><input type=\"button\" value=\"Logout\"/></a>");
				out.println("</body></html>");

				conn.commit();
			}
			catch (Exception ex) {
				conn.rollback();
				throw ex;
			}
			finally {
				conn.setAutoCommit(true);
			}
		}
		catch (Exception ex) {
			ex.printStackTrace();
		}
	}

	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		doGet(request, response);
	}
}
