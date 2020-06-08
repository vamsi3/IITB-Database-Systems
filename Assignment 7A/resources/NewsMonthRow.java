
//Set appropriate package name

import org.apache.spark.api.java.function.MapFunction;
import org.apache.spark.sql.Dataset;

import org.apache.spark.sql.Row;
import org.apache.spark.sql.RowFactory;
import org.apache.spark.sql.SparkSession;
import org.apache.spark.sql.catalyst.encoders.ExpressionEncoder;
import org.apache.spark.sql.catalyst.encoders.RowEncoder;
import org.apache.spark.sql.types.DataTypes;
import org.apache.spark.sql.types.StructType;

/**
 * This class uses Dataset APIs of spark to count number of articles per month
 * The year-month is obtained as a dataset of Row
 * */

public class NewsMonthRow {

	public static void main(String[] args) {
		
		//Input dir - should contain all input json files
		String inputPath="/users/ug16/<user_name>/Downloads/newsdata"; //Use absolute paths 
		
		//Ouput dir - this directory will be created by spark. Delete this directory between each run
		String outputPath="/users/ug16/<your_name>/output";   //Use absolute paths
		
		StructType structType = new StructType();
	    structType = structType.add("year-month", DataTypes.StringType, false); // false => not nullable
	    ExpressionEncoder<Row> dateRowEncoder = RowEncoder.apply(structType);
		
		SparkSession sparkSession = SparkSession.builder()
				.appName("Month wise news articles")		//Name of application
				.master("local")								//Run the application on local node
				.config("spark.sql.shuffle.partitions","2")		//Number of partitions
				.getOrCreate();
		
		//Read multi-line JSON from input files to dataset
		Dataset<Row> inputDataset=sparkSession.read().option("multiLine", true).json(inputPath);   
		
		
		// Apply the map function to extract the year-month
		Dataset<Row> yearMonthDataset=inputDataset.map(new MapFunction<Row,Row>(){
			public Row call(Row row) throws Exception {
				// The first 7 characters of date_published gives the year-month 
				String yearMonthPublished=((String)row.getAs("date_published")).substring(0, 7);

                // RowFactory.create() takes 1 or more parameters, and creates a row out of them.
				Row returnRow=RowFactory.create(yearMonthPublished);

				return returnRow;	  
			}
			
		}, dateRowEncoder);
		
		
		// Group by the desired column(s) and take count. groupBy() takes 1 or more parameters
		Dataset<Row> count=yearMonthDataset.groupBy("year-month").count().as("year_month_count");  
		
		
		//Outputs the dataset to the standard output
		//count.show();
		
		
		//Ouputs the result to a file
		count.toJavaRDD().saveAsTextFile(outputPath);	
		
	}
	
}
