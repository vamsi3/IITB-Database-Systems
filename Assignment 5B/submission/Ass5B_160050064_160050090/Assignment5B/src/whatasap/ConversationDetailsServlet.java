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

@WebServlet("/ConversationDetails")
public class ConversationDetailsServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;

    public ConversationDetailsServlet() {
        super();
    }

	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		if (!isSessionLoggedIn(request, response)) return;
		HttpSession session = request.getSession(false);
		String id = session.getAttribute("id").toString();
		String thread_id = request.getParameter("thread-id");

		// check user exists and fail accordingly.
		if (!UserExistsInThread(id, thread_id)) {
			response.sendError(HttpServletResponse.SC_FORBIDDEN);
			return;
		}

		int lastseen_post_id = GetLastSeenPostId(id, thread_id);
		
		response.setContentType("text/html");
		PrintWriter out = response.getWriter();
		out.println(String.format("<head><style> .scroll tbody { display:block; height:200px;  overflow:auto; } </style><script> window.onload = function() { document.getElementById(%d).scrollIntoView(); }; </script></head>", lastseen_post_id));
		out.println("<a href=\"Home\">Go Home</a><br>");

		addMessageList(thread_id, out);
		addCreateMessageForm(thread_id, out);
		UpdateLastSeenPostId(id, thread_id);
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

	private boolean UserExistsInThread(String id, String thread_id) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement chkUserQuery1 = conn.prepareStatement("SELECT uid1, uid2 FROM conversations WHERE thread_id=?");
			) {
				chkUserQuery1.setInt(1, Integer.parseInt(thread_id));
				ResultSet rset1 = chkUserQuery1.executeQuery();
				if (!rset1.next()) return false;
				String uid1 = rset1.getString(1);
				String uid2 = rset1.getString(2);
				if (uid1.equals(id) || uid2.equals(id)) return true;
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

	private void addCreateMessageForm(String thread_id, PrintWriter out) {
		out.println(String.format("<form action=\"NewMessage\" method=\"POST\">Enter a message:<input type=\"text\" name=\"message-text\"><input type=\"hidden\" value=\"%s\" name=\"thread-id\" /><input type=\"submit\" value=\"Send Message\"></form>", thread_id));
	}

	private void addMessageList(String thread_id, PrintWriter out) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement msgQuery1 = conn.prepareStatement("SELECT name, text, timestamp, post_id FROM posts, users WHERE posts.uid=users.uid AND thread_id=? ORDER BY timestamp");
			) {
				msgQuery1.setInt(1, Integer.parseInt(thread_id));
				ResultSet rset1 = msgQuery1.executeQuery();
				toHTML(rset1, out);
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

	private int GetLastSeenPostId(String id, String thread_id) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement lastseenQuery1 = conn.prepareStatement("SELECT lastseen_post_id FROM lastseen WHERE uid=? AND thread_id=?");
			) {
				lastseenQuery1.setString(1, id);
				lastseenQuery1.setInt(2, Integer.parseInt(thread_id));
				ResultSet rset1 = lastseenQuery1.executeQuery();
				if (rset1.next()) return rset1.getInt(1);
				else return -1;
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
			return -1;
		}
	}

	private void UpdateLastSeenPostId(String id, String thread_id) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement lastpostQuery1 = conn.prepareStatement("SELECT MAX(post_id) FROM posts WHERE thread_id=?");
				PreparedStatement lastseenUpdate1 = conn.prepareStatement("INSERT INTO lastseen VALUES (?, ?, ?) ON CONFLICT (uid, thread_id) DO UPDATE SET lastseen_post_id = ?;");
			) {
				lastpostQuery1.setInt(1, Integer.parseInt(thread_id));
				ResultSet rset1 = lastpostQuery1.executeQuery();
				int lastseen_post_id;
				if (rset1.next()) lastseen_post_id = rset1.getInt(1);
				else return;

				lastseenUpdate1.setString(1, id);
				lastseenUpdate1.setInt(2, Integer.parseInt(thread_id));
				lastseenUpdate1.setInt(3, lastseen_post_id);
				lastseenUpdate1.setInt(4, lastseen_post_id);
				lastseenUpdate1.executeUpdate();
			}
			catch (SQLException ex) {
				throw ex;
			}
			finally {
				conn.setAutoCommit(true);
			}
		}
		catch (SQLException ex) {
			return;
		}
	}

	private void toHTML(ResultSet rset, PrintWriter out) {
		ResultSetMetaData rsmd;
		try {
			rsmd = rset.getMetaData();
			out.println("<table class=\"scroll\">");
			out.print("\t<tr> ");
			out.println("<th>Sender</th> <th>Message</th> <th>Time</th>");
			out.println("</tr>");
			while(rset.next()) {
				int cnt = rsmd.getColumnCount();
				out.print(String.format("\t<tr id=%s> ", rset.getString(cnt)));
				for (int i=1; i<cnt; i++) {
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
