import java.sql.*;

public class Ass4APart1_160050064 {

	private static final String url = "jdbc:postgresql://localhost:5640/Test";
	private static final String user = "vamsikrishna";
	private static final String password = "";

	public static void main(String[] args) {

		try (java.sql.Connection conn = DriverManager.getConnection(url, user, password)) {
			conn.setAutoCommit(false);
			try (
				PreparedStatement stmt1 = conn.prepareStatement("SELECT grade, credits FROM takes, course WHERE takes.course_id = course.course_id AND ID=? AND takes.course_id=? AND sec_id=? AND semester=? AND year=?");
				PreparedStatement stmt2 = conn.prepareStatement("UPDATE takes SET grade=? WHERE ID=? AND course_id=? AND sec_id=? AND semester=? AND year=?");
				PreparedStatement stmt3 = conn.prepareStatement("UPDATE student SET tot_cred = tot_cred - ? WHERE ID = ?");
				PreparedStatement stmt4 = conn.prepareStatement("UPDATE student SET tot_cred = tot_cred + ? WHERE ID = ?");
			) {
				String new_grade = args[5];

				stmt2.setString(1, new_grade);
				for (int i=0; i<4; i++) {
					stmt1.setString(i+1, args[i]);
					stmt2.setString(i+2, args[i]);
				}
				stmt1.setInt(5, Integer.parseInt(args[4]));
				stmt2.setInt(6, Integer.parseInt(args[4]));

				ResultSet rset = stmt1.executeQuery();

				if (!rset.next()) {
					conn.rollback();
					throw new SQLException("No matching takes tuple found!");
				}

				int course_credits = rset.getInt(2);
				String old_grade = rset.getString(1);

				System.out.println(course_credits);

				stmt2.executeUpdate();

				if (new_grade.equals("F") && !(rset.wasNull() || old_grade.equals("F"))) {
					stmt3.setInt(1, course_credits);
					stmt3.setString(2, args[0]);
					stmt3.executeUpdate();
				}
				else if (!new_grade.equals("F") && (rset.wasNull() || old_grade.equals("F"))) {
					stmt4.setInt(1, course_credits);
					stmt4.setString(2, args[0]);
					stmt4.executeUpdate();
				}

				conn.commit();
			}
			catch (Exception ex) {
				conn.rollback();
				throw ex;
			}
			finally {
				conn.setAutoCommit(true);
			}
		}
		catch (Exception e) {
			e.printStackTrace();
		}
	}
}
