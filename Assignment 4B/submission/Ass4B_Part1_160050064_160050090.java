import java.sql.*;
import java.util.*;
import javax.json.*;

public class Ass4B_Part1_160050064_160050090 {
    private static final String url = "jdbc:postgresql://localhost:5432/postgres";
    private static final String user = "postgres";
    private static final String password = "";

	public static void main(String[] args) {
		try (
				Connection conn = DriverManager.getConnection(url, user, password);
				Statement stmt = conn.createStatement();
			) {
			conn.setAutoCommit(false);
			
			Scanner sc = new Scanner(System.in);
			String query = sc.nextLine();
			sc.close();
			ResultSet rset;
			try {
				rset = stmt.executeQuery(query);
			}
			catch (Exception ex) {
				System.out.println("Error with query");
				return;
			}
			
			// Uncomment as required
			// toHTML(rset);
			toJSON(rset);

			conn.commit();
			conn.setAutoCommit(true);
		}
		catch (Exception ex) {
			System.out.println("Exception : " + ex);
		}
	}
	
	private static void toHTML(ResultSet rset) {
		ResultSetMetaData rsmd;
		try {
			rsmd = rset.getMetaData();
			System.out.println("<table>");
			System.out.print("\t<tr> ");
			for (int i=1; i<=rsmd.getColumnCount(); i++) {
				System.out.print("<th>"+rsmd.getColumnName(i)+"</th> ");
			}
			System.out.println("</tr>");
			while(rset.next()) {
				System.out.print("\t<tr> ");
				for (int i=1; i<=rsmd.getColumnCount(); i++) {
					System.out.print("<td>"+rset.getString(i)+"</td> ");
				}
				System.out.println("</tr>");
			}
			System.out.println("</table>");
		}
		catch (SQLException sqle) {
			sqle.printStackTrace();
		}
	}

	private static JsonObject toJSON(ResultSet rset) throws SQLException {
		ResultSetMetaData rsmd;
		JsonObject resultObj;
		rsmd = rset.getMetaData();
		JsonObjectBuilder result = Json.createObjectBuilder();

		JsonArrayBuilder header = Json.createArrayBuilder();
		JsonArrayBuilder data = Json.createArrayBuilder();
		
		for(int i=1;i<=rsmd.getColumnCount();i++) {
			header.add(rsmd.getColumnName(i));
		}
		result.add("header", header.build());
		
		while(rset.next()) {
			JsonObjectBuilder row = Json.createObjectBuilder();
			for(int i=1;i<=rsmd.getColumnCount();i++) {
				String s = rset.getString(i);
				if (rset.wasNull()) {
					row.add(rsmd.getColumnName(i),"null" );
				}
				else {
					row.add(rsmd.getColumnName(i),s );
				}
			}
			data.add(row.build());
		}
		result.add("data", data.build());
		resultObj = result.build();
		System.out.println(resultObj.toString());
		return resultObj;
		
		// ResultSetMetaData rsmd;
		// try {
		// 	rsmd = rset.getMetaData();

		// 	System.out.print("{\"header\": [");
		// 	for (int i=1; i<=rsmd.getColumnCount(); i++) {
		// 		System.out.print("\""+rsmd.getColumnName(i)+"\"");
		// 		if (i != rsmd.getColumnCount()) {
		// 			System.out.print(", ");
		// 		}
		// 	}
		// 	System.out.println("],");
		// 	System.out.print("\"data\": ");
		// 	System.out.print("[ ");
			
		// 	int f=0;
		// 	while(rset.next()) {
		// 		if (f!=0) {
		// 			System.out.println(",");
		// 		}
		// 		System.out.print("{");
		// 		for(int i=1;i<=rsmd.getColumnCount();i++) {
		// 			System.out.print(String.format("\"%s\":\"%s\"", rsmd.getColumnName(i), rset.getString(i)));
		// 			if (i != rsmd.getColumnCount()) {
		// 				System.out.print(", ");
		// 			}
		// 		}
		// 		System.out.print("}");
		// 		f = 1;
		// 	}
		// 	System.out.println("\n]\n}");
		// }
		// catch (SQLException sqle) {
		// 	sqle.printStackTrace();
		// }

	}
}
