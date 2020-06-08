package whatasap;

import java.io.IOException;
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


@WebServlet("/NewMessage")
public class NewMessageServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;

    public NewMessageServlet() {
        super();
    }

	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		doPost(request, response);
	}

	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		if (!isSessionLoggedIn(request, response)) return;
		HttpSession session = request.getSession(false);
		String id = session.getAttribute("id").toString();
		String thread_id = request.getParameter("thread-id");
		String message_text = request.getParameter("message-text");

		if (thread_id == null || message_text == null) {
			response.sendError(HttpServletResponse.SC_BAD_REQUEST);
			return;
		}

		// check user exists and fail accordingly.
		if (!UserExistsInThread(id, thread_id)) {
			response.sendError(HttpServletResponse.SC_FORBIDDEN);
			return;
		}

		addNewMessageToDatabase(id, thread_id, message_text);
		response.sendRedirect(String.format("ConversationDetails?thread-id=%s", thread_id));
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

	private void addNewMessageToDatabase(String id, String thread_id, String message_text) {
		try (java.sql.Connection conn = DriverManager.getConnection(Config.DATABASE_URL, Config.DATABASE_USERNAME, Config.DATABASE_PASSWORD)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement addMsgUpdate1 = conn.prepareStatement("INSERT INTO posts(thread_id, uid, timestamp, text) VALUES (?, ?, CURRENT_TIMESTAMP, ?)");
			) {
				addMsgUpdate1.setInt(1, Integer.parseInt(thread_id));
				addMsgUpdate1.setString(2, id);
				addMsgUpdate1.setString(3, message_text);
				addMsgUpdate1.executeUpdate();
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
}
