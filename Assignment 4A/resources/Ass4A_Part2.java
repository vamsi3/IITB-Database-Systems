package ass;

import java.sql.*;
import java.util.Scanner;

public class Ass4A_Part2 {
    private static final String url = "jdbc:postgresql://localhost:5990/postgres";
    private static final String user = "sudarsha";
    private static final String password = "";

    public static void main(String[] args) {
        Scanner sc = new Scanner(System.in);
        System.out.print("ID: ");
        String i_id = sc.nextLine();

        noPrepStmt(i_id);
//        withPrepStmt(i_id);
    }

    private static void noPrepStmt(String id) {

        try (Connection conn = DriverManager.getConnection(url, user, password))
        {
            conn.setAutoCommit(false);

            try(Statement stmt = conn.createStatement()) {
                String query = "update instructordup " +
                        "set salary = salary * 1.10 " +
                        "where id = '" + id + "'";
                stmt.executeUpdate(query);
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
            e.printStackTrace();
        }
    }

    private static void withPrepStmt(String id) {

    }
}
