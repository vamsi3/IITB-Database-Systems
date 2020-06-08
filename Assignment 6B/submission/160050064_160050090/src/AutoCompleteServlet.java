import java.io.IOException;
import java.io.PrintWriter;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import java.sql.*;


/**
 * Servlet implementation class AutoCompleteServlet
 */
@WebServlet("/AutoCompleteServlet")
public class AutoCompleteServlet extends HttpServlet {
	private static final long serialVersionUID = 1L;
       
    /**
     * @see HttpServlet#HttpServlet()
     */
    public AutoCompleteServlet() {
        super();
        // TODO Auto-generated constructor stub
    }
    
    public static ObjectMapper mapper = new ObjectMapper();

	/**
	 * @see HttpServlet#doGet(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doGet(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		HttpSession session = request.getSession();
		
		response.setContentType("text/html");
		PrintWriter out = response.getWriter();
		
		
		
		
		if(session.getAttribute("id") == null) { //not logged in
			response.sendRedirect("LoginServlet");
		}
		
		String match = request.getParameter("term");
		match = match.toLowerCase();
		String query = "select * from users";
		
		
		ArrayNode json = null;
    	try (Connection conn = DriverManager.getConnection(Config.url, Config.user, Config.password))
        {
            conn.setAutoCommit(false);
            try(PreparedStatement stmt = conn.prepareStatement(query)) {
                ResultSet rs = stmt.executeQuery();
                json = resultSetToJson(rs,match);
                conn.commit();
            }
            catch(Exception ex)
            {
                conn.rollback();
                throw ex;
            }
            finally{
                conn.setAutoCommit(true);
            }
        } catch (Exception e) {
            out.print(errorJson(e.getMessage()).toString());
        }
    	
    	out.print(json.toString());
	
	}
	
	
	public static ArrayNode resultSetToJson(ResultSet rs, String match) throws SQLException {
		ArrayNode arr = mapper.createArrayNode();

		ResultSetMetaData rsmd = rs.getMetaData();
		while(rs.next()) {
			int numColumns = rsmd.getColumnCount();
			ObjectNode obj = mapper.createObjectNode();
			
			String s="";
			int flag = 0;
 			for (int i=1; i<numColumns+1; i++) {
 				String s1 = rs.getString(i);
				s+= s1;
				s+=" ";
				
				s1 = s1.toLowerCase();
				if(s1.startsWith(match)) {
					flag++;
				}
				
			}
 			
 			if(flag > 0) {
 				obj.put("label", s);
 	 			obj.put("value", rs.getString(1));
 				arr.add(obj);
 			}
		}
		return arr;
	}
	
	

	/**
	 * @see HttpServlet#doPost(HttpServletRequest request, HttpServletResponse response)
	 */
	protected void doPost(HttpServletRequest request, HttpServletResponse response) throws ServletException, IOException {
		// TODO Auto-generated method stub
		doGet(request, response);
	}
	
	public static ObjectNode errorJson(String errorMsg) {
		ObjectNode node = mapper.createObjectNode();
		node.put("status", false);
		node.put("message", errorMsg);
		return node;
	}
}
