import java.sql.Connection;

import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

public class DbHelper {
	public static final String DATA_LABEL = "data";
	public static final String MSG_LABEL = "message";
	public static final String STATUS_LABEL = "status";
	
	public static ObjectMapper mapper = new ObjectMapper();
	
	protected static enum ParamType{
		STRING,
		INT
	}
	
	/**
	 * Execute a query and return results as a list of lists
	 */
	protected static List<List<Object>> executeQueryList(String query, ParamType[] paramTypes, Object[] params) {
    	ResultSet rs = null;
    	List<List<Object>> res = new ArrayList<>();
    	try (Connection conn = DriverManager.getConnection(Config.url, Config.user, Config.password))
        {
            conn.setAutoCommit(false);
            try(PreparedStatement stmt = conn.prepareStatement(query)) {
            	setParams(stmt, paramTypes, params);
                rs = stmt.executeQuery();
                ResultSetMetaData rsmd = rs.getMetaData();
                while(rs.next()) {
                	List<Object> row = new ArrayList<>();
                	for(int i=1;i<=rsmd.getColumnCount();i++) {
                		row.add(rs.getObject(i));
                	}
                	res.add(row);
                }
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
    	
    	return res;
    }
	
	/**
	 * Executes query and returns results as JSON,
	 * returns null on any error.
	 */
	protected static String executeQueryJson(String query, ParamType[] paramTypes, Object[] params) {
    	ArrayNode json = null;
    	try (Connection conn = DriverManager.getConnection(Config.url, Config.user, Config.password))
        {
            conn.setAutoCommit(false);
            try(PreparedStatement stmt = conn.prepareStatement(query)) {
            	setParams(stmt, paramTypes, params);
                ResultSet rs = stmt.executeQuery();
                json = resultSetToJson(rs);
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
            return errorJson(e.getMessage()).toString();
        }
    	
    	ObjectNode node = mapper.createObjectNode();
    	node.putArray(DATA_LABEL).addAll(json);    	
    	node.put(STATUS_LABEL, true);
    	return node.toString();
    }
	
	/**
	 * Returns number of records updated in JSON format
	 * { "value" : <number of records updated> }
	 */
	protected static String executeUpdateJson(String updateQuery, ParamType[] paramTypes, Object[] params) {
    	int recordsUpdated = 0;
    	try (Connection conn = DriverManager.getConnection(Config.url, Config.user, Config.password))
        {
            conn.setAutoCommit(false);
            try(PreparedStatement stmt = conn.prepareStatement(updateQuery)) {
            	setParams(stmt, paramTypes, params);
            	recordsUpdated = stmt.executeUpdate();
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
            return errorJson(e.getMessage()).toString();
        }

    	boolean status = recordsUpdated == 0 ? false : true;
    	ObjectNode node = mapper.createObjectNode();
    	node.put(STATUS_LABEL, status);
    	return node.toString();
    }

	private static void setParams(PreparedStatement stmt,
			ParamType[] paramTypes, 
			Object[] params) throws SQLException {
		List<ParamType> paramTypesList = Arrays.asList(paramTypes);
		List<Object> paramsList = Arrays.asList(params);
		
		for(int i=0;i<paramsList.size();i++) {
			ParamType type = paramTypesList.get(i);
			Object param = paramsList.get(i);
			
			if(type.equals(ParamType.STRING)) {
				stmt.setString(i+1, (String)param);
			}
			else if(type.equals(ParamType.INT)) {
				stmt.setInt(i+1, (Integer)param);
			}
		}
	}
	
	/**
	 * Returns the results as a JSON array object.
	 * Use toString() on the result to get JSON string.
	 */
	public static ArrayNode resultSetToJson(ResultSet rs) throws SQLException {
		ArrayNode arr = mapper.createArrayNode();

		ResultSetMetaData rsmd = rs.getMetaData();
		while(rs.next()) {
			int numColumns = rsmd.getColumnCount();
			ObjectNode obj = mapper.createObjectNode();
			
 			for (int i=1; i<numColumns+1; i++) {
				String column_name = rsmd.getColumnName(i);
				if(rs.getObject(column_name) == null) {
					obj.putNull(column_name);
					continue;
				}
				
				if(rsmd.getColumnType(i)==java.sql.Types.BIGINT){
					obj.put(column_name, rs.getInt(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.BOOLEAN){
					obj.put(column_name, rs.getBoolean(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.DOUBLE){
					obj.put(column_name, rs.getDouble(column_name)); 
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.FLOAT){
					obj.put(column_name, rs.getFloat(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.INTEGER){
					obj.put(column_name, rs.getInt(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.NVARCHAR){
					obj.put(column_name, rs.getNString(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.VARCHAR){
					obj.put(column_name, rs.getString(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.TINYINT){
					obj.put(column_name, rs.getInt(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.SMALLINT){
					obj.put(column_name, rs.getInt(column_name));
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.DATE){
					obj.put(column_name, rs.getDate(column_name).toString());
				}
				else if(rsmd.getColumnType(i)==java.sql.Types.TIMESTAMP){
					obj.put(column_name, rs.getTimestamp(column_name).toString());   
				}
				else{
					obj.put(column_name, rs.getObject(column_name).toString());
				}
			}
			arr.add(obj);
		}
		return arr;
	}
	
	public static ObjectNode errorJson(String errorMsg) {
		ObjectNode node = mapper.createObjectNode();
		node.put(STATUS_LABEL, false);
		node.put(MSG_LABEL, errorMsg);
		return node;
	}
	
	public static ObjectNode okJson() {
    	ObjectNode node = mapper.createObjectNode();
    	node.put(STATUS_LABEL, true);
    	return node;
	}
	
	/**
	 * main() method for testing the functionality
	 * of other methods defined in DbHelper.
	 */
	public static void main(String[] args) throws SQLException {
		String json = DbHelper.executeQueryJson("select * from student", 
				new DbHelper.ParamType[] {}, 
				new Object[] {});
		if(json != null) {
			System.out.println(json);
		}
	}

}
