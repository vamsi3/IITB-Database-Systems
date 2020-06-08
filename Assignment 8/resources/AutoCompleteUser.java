import java.io.IOException;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.ObjectMapper;

/**
 * Servlet implementation class AutoCompleteUser.
 * Based on the submission by 160070017 and 160070021 for Assignment 6.
 * Modified for Flutter assignment.
 */
@WebServlet("/AutoCompleteUser")
public class AutoCompleteUser extends HttpServlet {
	private static final long serialVersionUID = 1L;

	/**
	 * @see HttpServlet#HttpServlet()
	 */
	public AutoCompleteUser() {
		super();
	}

	/**
	 * @see HttpServlet#doGet(HttpServletRequest request, HttpServletResponse
	 *      response)
	 */
	protected void doGet(HttpServletRequest request, HttpServletResponse response)
			throws ServletException, IOException {
		HttpSession session = request.getSession();
		if(session.getAttribute("id") == null) { //not logged in
			response.getWriter().print(DbHelper.errorJson("Not logged in").toString());
			return;
		}
		String substr = request.getParameter("term");

		String query = "select concat(\'uid: \', uid, \', name: \', name, \', phone: \', phone) as label, uid as value from users where (uid like ? or name like ? or phone like ?)";

		String json = DbHelper.executeQueryJson(query, new DbHelper.ParamType[] { DbHelper.ParamType.STRING,
				DbHelper.ParamType.STRING, DbHelper.ParamType.STRING },
				new String[] { substr + "%", substr + "%", substr + "%" });

		ObjectMapper objectMapper = new ObjectMapper();
		Object jsondata = objectMapper.readValue(json, ObjectNode.class);
		response.getWriter().print(((ObjectNode) jsondata).get("data"));
	}

	/**
	 * @see HttpServlet#doPost(HttpServletRequest request, HttpServletResponse
	 *      response)
	 */
	protected void doPost(HttpServletRequest request, HttpServletResponse response)
			throws ServletException, IOException {
		doGet(request, response);
	}

}

/*
"select concat(\'uid: \', uid, \', name: \', name, \', phone: \', phone) as label, uid as value from users where (uid like ? or name like ? or phone like ?)";
 */
