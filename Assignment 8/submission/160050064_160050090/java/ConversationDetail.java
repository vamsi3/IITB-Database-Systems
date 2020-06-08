
import java.io.IOException;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

/**
 * Servlet implementation class ConversationDetail
 */
@WebServlet("/ConversationDetail")
public class ConversationDetail extends HttpServlet {
	private static final long serialVersionUID = 1L;
       
    /**
     * @see HttpServlet#HttpServlet()
     */
    public ConversationDetail() {
        super();
        // TODO Auto-generated constructor stub
    }

	/**
	 * @see HttpServlet#doGet(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doGet(HttpServletRequest request, HttpServletResponse response) 
			throws ServletException, IOException {
		HttpSession session = request.getSession();
		if(session.getAttribute("id") == null) { //not logged in
			response.getWriter().print(DbHelper.errorJson("Not logged in").toString());
			return;
		}
		String id = (String) session.getAttribute("id");
		String other_id = request.getParameter("other_id");
		
		String messagesQuery = "with temp(post_id,thread_id,uid,timestamp,text) as "+
				"(select p.* "
				+ "from posts p, conversations c "
				+ "where p.thread_id = c.thread_id "
				+ "and ((c.uid1 = ? and c.uid2 = ?) or "
				+ "		(c.uid2 = ? and c.uid1 = ?)) "
				+ "order by p.timestamp asc) " 
				+ "select temp.*, Users.name "
				+ "from temp, Users "
				+ "where temp.uid = Users.uid";
		String json = DbHelper.executeQueryJson(messagesQuery, 
				new DbHelper.ParamType[] {DbHelper.ParamType.STRING,
						DbHelper.ParamType.STRING,
						DbHelper.ParamType.STRING,
						DbHelper.ParamType.STRING}, 
				new String[] {id, other_id, id, other_id});
		response.getWriter().print(json);
	}

	/**
	 * @see HttpServlet#doPost(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		// TODO Auto-generated method stub
		doGet(request, response);
	}
	
	public static void main(String[] args) throws ServletException, IOException {
		new ConversationDetail().doGet(null, null);
	}

}
