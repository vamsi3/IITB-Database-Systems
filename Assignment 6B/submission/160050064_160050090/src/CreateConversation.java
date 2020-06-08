
import java.io.IOException;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

/**
 * Servlet implementation class CreateConversation
 */
@WebServlet("/CreateConversation")
public class CreateConversation extends HttpServlet {
	private static final long serialVersionUID = 1L;
       
    /**
     * @see HttpServlet#HttpServlet()
     */
    public CreateConversation() {
        super();
        // TODO Auto-generated constructor stub
    }

	/**
	 * @see HttpServlet#doGet(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		HttpSession session = request.getSession();
		if(session.getAttribute("id") == null) { //not logged in
			response.sendRedirect("LoginServlet");
		}
		
		String id = (String) session.getAttribute("id");
		String other_id = (String) request.getParameter("other_id");
		
		//ensure order
		String uid1, uid2;
		if(id.compareTo(other_id) < 0) {
			uid1 = id;
			uid2 = other_id;
		}
		else {
			uid1 = other_id;
			uid2 = id;
		}
		
		String query = "insert into conversations values (?, ?)";
		String json = DbHelper.executeUpdateJson(query, 
				new DbHelper.ParamType[] {DbHelper.ParamType.STRING,  DbHelper.ParamType.STRING},
				new String[] {uid1, uid2});
		
		response.getWriter().print(json);
	}

	/**
	 * @see HttpServlet#doPost(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doPost(HttpServletRequest request, HttpServletResponse response) 
			throws ServletException, IOException {
		doGet(request, response);
	}

}
