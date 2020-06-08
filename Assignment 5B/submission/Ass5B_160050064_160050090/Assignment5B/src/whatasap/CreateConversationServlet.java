package whatasap;

import java.io.IOException;
import java.io.PrintWriter;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;


@WebServlet("/CreateConversation")
public class CreateConversationServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;

    public CreateConversationServlet() {
        super();
    }

	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		doPost(request, response);
	}

	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		if (!isSessionLoggedIn(request, response)) return;
		HttpSession session = request.getSession(false);
		String id = session.getAttribute("id").toString();
		String friend_id = request.getParameter("friend-id");

		if (friend_id == null) {
			response.sendError(HttpServletResponse.SC_BAD_REQUEST);
			return;
		}

		response.setContentType("text/html");
		PrintWriter out = response.getWriter();
		if (!userIDExists(friend_id)) out.println("Failed! No such user exists...");
		else if (id.equals(friend_id)) out.println("You entered your userID... Are you so alone? Sad :/");
		else if (ConversationExists(id, friend_id)) out.println("Failed! User already in a conversation with you...");
		else if (createNewConversationInDatabase(id, friend_id)) out.println("Success! New conversation created.");
		else out.println("Failed! Something is wrong...");
		out.println("<br><a href=\"Home\">Go Home</a>");
	}

	private boolean isSessionLoggedIn(HttpServletRequest request, HttpServletResponse response) throws IOException {
		HttpSession session = request.getSession(false);
		if (session == null) {
			response.sendRedirect("Login");
			return false;
		}
		return true;
	}

	private boolean userIDExists(String uid) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement chkUserIDQuery1 = conn.prepareStatement("SELECT uid FROM users WHERE uid=?");
			) {
				chkUserIDQuery1.setString(1, uid);
				ResultSet rset1 = chkUserIDQuery1.executeQuery();
				return rset1.next()? true: false;
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

	private boolean ConversationExists(String id, String friend_id) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement chkConvQuery1 = conn.prepareStatement("SELECT * FROM conversations WHERE uid1=? AND uid2=?");
			) {
				if (friend_id.compareTo(id) < 0) {
					String temp = id;
					id = friend_id;
					friend_id = temp;
				}
				chkConvQuery1.setString(1, id);
				chkConvQuery1.setString(2, friend_id);
				ResultSet rset1 = chkConvQuery1.executeQuery();
				return rset1.next()? true: false;
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

	private boolean createNewConversationInDatabase(String id, String friend_id) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement addConvUpdate1 = conn.prepareStatement("INSERT INTO conversations VALUES (?, ?)");
			) {
				if (friend_id.compareTo(id) < 0) {
					String temp = id;
					id = friend_id;
					friend_id = temp;
				}
				addConvUpdate1.setString(1, id);
				addConvUpdate1.setString(2, friend_id);
				System.out.println(id + " " + friend_id);
				addConvUpdate1.executeUpdate();
				return true;
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
