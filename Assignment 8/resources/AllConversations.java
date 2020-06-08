import java.io.IOException;
import java.io.PrintWriter;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

/**
 * Servlet implementation class AllConversations
 */
@WebServlet("/AllConversations")
public class AllConversations extends HttpServlet {
	private static final long serialVersionUID = 1L;
       
    /**
     * @see HttpServlet#HttpServlet()
     */
    public AllConversations() {
        super();
        // TODO Auto-generated constructor stub
    }

	/**
	 * @see HttpServlet#doGet(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		HttpSession session = request.getSession();
		if(session.getAttribute("id") == null) { //not logged in
			response.getWriter().print(DbHelper.errorJson("Not logged in").toString());
			return;
		}
		
		String userid = (String) session.getAttribute("id");
		String query = 
				"with max_ts as " + 
				"	(select c.uid1, c.uid2, max(p.timestamp) as last_timestamp, count(text) as num_msgs " + 
				" 	 from conversations c " + 
				"         	left outer join posts p on c.thread_id = p.thread_id " + 
				"  	 group by c.uid1, c.uid2) " + 
				"select case " + 
				"         when c.uid1 = ? then c.uid2 " + 
				"         else c.uid1 " + 
				"       end as uid, m.last_timestamp, m.num_msgs " + 
				"from conversations c natural join max_ts m " +
				"where c.uid1 = ? or c.uid2 = ?" + 
				"order by num_msgs > 0 desc, last_timestamp desc";
		String res = DbHelper.executeQueryJson(query, 
				new DbHelper.ParamType[] {DbHelper.ParamType.STRING, 
						DbHelper.ParamType.STRING,
						DbHelper.ParamType.STRING}, 
				new String[] {userid, userid, userid});
		
		PrintWriter out = response.getWriter();
		out.print(res);
	}

	/**
	 * @see HttpServlet#doPost(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		// TODO Auto-generated method stub
		doGet(request, response);
	}
	
	/**
	 * For testing other methods in this class.
	 */
	public static void main(String[] args) throws ServletException, IOException {
		new AllConversations().doGet(null, null);
	}

}
