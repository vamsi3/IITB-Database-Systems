import java.sql.*;
import java.util.*;

public class Ass4B_Part2_160050064_160050090 {
    private static final String url = "jdbc:postgresql://localhost:5432/postgres";
    private static final String user = "postgres";
    private static final String password = "";

	public static void main(String[] args) {
		try (
				Connection conn = DriverManager.getConnection(url, user, password);
				Statement stmt = conn.createStatement();
			) {
		   conn.setAutoCommit(false);
		   
		   ResultSet rset1 = stmt.executeQuery("with recursive heights(id,height) as " + 
				"(select id,1 as height from part where id not in (select pid from subpart) " + 
				" union " + 
				" select pID, " + 
				" heights.height+1 " + 
				" from subpart as s, heights " + 
				" where s.spID = heights.id and heights.height < 100 " + 
				") " + 
				"select id,max(height) as height " + 
				"from heights group by id order by height asc;");
		   
			Statement stmt_n = conn.createStatement();		   
			stmt_n.executeUpdate("create temporary table cost(id VARCHAR, cost INT);");

			while (rset1.next()) {
				String id=rset1.getString(1);
				PreparedStatement stmt0 = conn.prepareStatement("select cost from part where id = ?;");
				stmt0.setString(1,id);
				ResultSet temp = stmt0.executeQuery();
				int cost=0;
				while (temp.next()) {
					cost = temp.getInt(1);
				}
	
				PreparedStatement stmt1 = conn.prepareStatement("select spID,number from subpart where pID = ?;");
				stmt1.setString(1,id);
				ResultSet rset2 = stmt1.executeQuery();
				while (rset2.next()) {
					String child = rset2.getString(1);
					int n = rset2.getInt(2);
					PreparedStatement stmt2 = conn.prepareStatement("select cost from cost where id = ?;");
					stmt2.setString(1,child);
					ResultSet temp1 = stmt2.executeQuery();
					while(temp1.next()) {
						cost+=n*temp1.getInt(1);
					}
				}
				PreparedStatement stmt3 = conn.prepareStatement("insert into cost values(?,?);");
				stmt3.setString(1,id);
				stmt3.setInt(2, cost);
				stmt3.executeUpdate();
			}

			Scanner sc = new Scanner(System.in);
			while (sc.hasNextLine()) {
				String id = sc.nextLine();
				PreparedStatement stmt4 = conn.prepareStatement("select cost from cost where id = ?;");
				stmt4.setString(1, id);
				ResultSet rset4 = stmt4.executeQuery();
				if (rset4.next()) {
					int cost = rset4.getInt(1);
					System.out.println(String.format("%s %d", id, cost));
				}
				else {
					System.out.println(String.format("Item with ID %s not found", id));
				}
			}
			sc.close();

			conn.commit();
			conn.setAutoCommit(true);
		}
		catch (SQLException sqle) {
			sqle.printStackTrace();
		}
	}
}