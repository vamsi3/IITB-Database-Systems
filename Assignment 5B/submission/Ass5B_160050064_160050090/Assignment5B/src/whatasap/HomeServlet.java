package whatasap;

import java.io.IOException;
import java.io.PrintWriter;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;


@WebServlet("/Home")
public class HomeServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;

    public HomeServlet() {
        super();
    }

	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		if (!isSessionLoggedIn(request, response)) return;
		HttpSession session = request.getSession(false);
		String id = session.getAttribute("id").toString();
		response.setContentType("text/html");
		PrintWriter out = response.getWriter();
		addCreateConversationForm(out);
		addUserConversationList(id, out);
	}

	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		doGet(request, response);
	}

	private boolean isSessionLoggedIn(HttpServletRequest request, HttpServletResponse response) throws IOException {
		HttpSession session = request.getSession(false);
		if (session == null) {
			response.sendRedirect("Login");
			return false;
		}
		return true;
	}

	private void addCreateConversationForm(PrintWriter out) {
		out.println("<form action=\"CreateConversation\" method=\"POST\">Enter a user ID:<input type=\"text\" name=\"friend-id\"><input type=\"submit\" value=\"Create Conversation\"></form>");
		// logout
		out.println("<a href=\"Logout\"><input type=\"button\" value=\"Logout\"/></a>");
	}

	private void addUserConversationList(String id, PrintWriter out) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement convQuery1 = conn.prepareStatement("with friend(name,thread_id) as" + 
						" (select u1.name, thread_id from conversations, users as u1 where conversations.uid2 = ? and conversations.uid1 = u1.uid" + 
						" union" + 
						" select u2.name, thread_id from conversations, users as u2 where conversations.uid1 = ? and conversations.uid2 = u2.uid)," + 
						" messages(name,text,timestamp,thread_id) as" + 
						" (select name,text, timestamp,friend.thread_id from friend,posts where friend.thread_id = posts.thread_id)" + 
						" select name,text,timestamp,thread_id from messages where timestamp in (select max(timestamp) from messages group by name) order by timestamp desc"
						);
				PreparedStatement convQuery2 = conn.prepareStatement("with friend(name,thread_id) as" + 
						" (select u1.name, thread_id from conversations, users as u1 where conversations.uid2 = ? and conversations.uid1 = u1.uid" + 
						" union" + 
						" select u2.name, thread_id from conversations, users as u2 where conversations.uid1 = ? and conversations.uid2 = u2.uid)," + 
						" messages(name,text,thread_id) as" + 
						" (select name,text,friend.thread_id from friend left outer join posts on (friend.thread_id = posts.thread_id))" + 
						" select name,thread_id from messages where text is null"
						);
			) {
				convQuery1.setString(1, id);
				convQuery1.setString(2, id);
				ResultSet rset1 = convQuery1.executeQuery();
				toHTML(rset1, out, false);
				convQuery2.setString(1, id);
				convQuery2.setString(2, id);
				ResultSet rset2 = convQuery2.executeQuery();
				toHTML(rset2, out, true);
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
		}
	}

	private void toHTML(ResultSet rset, PrintWriter out, boolean isNull) {
		ResultSetMetaData rsmd;
		try {
			rsmd = rset.getMetaData();
			out.println("<table>");
			out.print("\t<tr> ");
			if (isNull) out.print("<th>Name</th> <th>Conversation Link</th>");
			else out.print("<th>Name</th> <th>Latest Message</th> <th>Time</th> <th>Conversation Link</th>");
			out.println("</tr>");
			while(rset.next()) {
				out.print("\t<tr> ");
				for (int i=1; i<=rsmd.getColumnCount(); i++) {
					String clickText = "Click to " + (isNull? "send first message": "Open");
					if (i == rsmd.getColumnCount()) out.print(String.format("<td><a href=\"ConversationDetails?thread-id=%s\">%s</a></td>", rset.getString(i), clickText));
					else out.print("<td>"+rset.getString(i)+"</td> ");

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
