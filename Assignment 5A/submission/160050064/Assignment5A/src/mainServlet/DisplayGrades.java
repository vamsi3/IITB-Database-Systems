package mainServlet;

import java.io.IOException;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.*;
import java.io.PrintWriter;
import java.sql.*;

@WebServlet("/DisplayGrades")
public class DisplayGrades extends HttpServlet {
	private static final long serialVersionUID = 1L;
	private static final String databaseUrl = "jdbc:postgresql://localhost:5640/test";
	private static final String databaseUsername = "labuser";
	private static final String databasePassword = "";

    public DisplayGrades() {
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
				PreparedStatement takesQuery = conn.prepareStatement("SELECT takes.course_id, title, sec_id, semester, year, grade FROM takes, course WHERE id=? AND takes.course_id=course.course_id");
			) {
				response.setContentType("text/html");
				PrintWriter out = response.getWriter();
				out.println("<html><body>");

				takesQuery.setString(1, ID);
				ResultSet rset = takesQuery.executeQuery();

				toHTML(rset, out);

				out.println("<br><a href=\"Home\"><input type=\"button\" value=\"Home\"/></a>");
				out.println("</body></html>");
			}
			catch (Exception ex) {
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

	private void toHTML(ResultSet rset, PrintWriter out) {
		ResultSetMetaData rsmd;
		try {
			rsmd = rset.getMetaData();
			out.println("<table>");
			out.print("<tr> ");
			for (int i=1; i<=rsmd.getColumnCount(); i++) {
				out.print("<th>"+rsmd.getColumnName(i)+"</th> ");
			}
			out.println("</tr>");
			while(rset.next()) {
				out.print("<tr> ");
				for (int i=1; i<=rsmd.getColumnCount(); i++) {
					out.print("<td>"+rset.getString(i)+"</td> ");
				}
				out.println("</tr>");
			}
			out.println("</table>");
		}
		catch (SQLException sqle) {
			sqle.printStackTrace();
		}
	}
}
